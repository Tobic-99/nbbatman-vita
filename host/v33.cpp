// NEC V33/V30 interpreter (our own implementation, written from the 8086/80186 instruction set and NEC's V-series
// documentation). NEC behaviour where it differs from Intel: AAM/AAD ignore their operand (always base 10),
// 0F is a prefix for the NEC-only instructions (no POP CS), 64/65 are REPNC/REPC, the flag word reads with bits
// 12-15 set, shift counts are not masked.
#include "v33.h"

#include "v33_exec.h"

using namespace v33x;


void V33::reset() {
    for (auto& x : r) x = 0;
    sreg[ES] = sreg[SS] = sreg[DS] = 0;
    sreg[CS] = 0xffff;
    ip = 0;
    CF = PF = AF = ZF = SF = OF = DF = IF = TF = false;
    halted = false;
    irq_line = nmi_pending = false;
    pending_int = -1;
}

uint16_t V33::flags() const {
    return 0xf002 | CF | (PF << 2) | (AF << 4) | (ZF << 6) | (SF << 7) | (TF << 8) | (IF << 9) | (DF << 10) | (OF << 11);
}
void V33::set_flags(uint16_t f) {
    CF = f & 1; PF = f & 4; AF = f & 0x10; ZF = f & 0x40; SF = f & 0x80;
    TF = f & 0x100; IF = f & 0x200; DF = f & 0x400; OF = f & 0x800;
}

// ---------------------------------------------------------------------------------------------------------------
// The interpreter proper lives in a helper with access to the CPU; kept in one translation unit.

void V33::interrupt(int vector) {
    Exec e(*this);
    e.push(flags());
    e.push(sreg[CS]);
    e.push(ip);
    IF = TF = false;
    uint32_t a = (uint32_t)(vector & 0xff) * 4;
    ip = (uint16_t)(rd8(a) | (rd8(a + 1) << 8));
    sreg[CS] = (uint16_t)(rd8(a + 2) | (rd8(a + 3) << 8));
    halted = false;
    if (on_call) on_call(sreg[CS], ip, true);
}

int V33::step() {
    if (nmi_pending) { nmi_pending = false; interrupt(2); return 20; }
    if (pending_int >= 0) { int v = pending_int; pending_int = -1; interrupt(v); return 20; }
    if (IF && irq_line) { interrupt(irq_ack ? irq_ack() : 0); return 20; }
    if (halted) return 4;
    return exec_one();
}

int V33::exec_one() {
    Exec e(*this);
    for (;;) {   // prefixes
        uint8_t b = e.fetch_op();
        switch (b) {
        case 0x26: e.in.seg = ES; continue;
        case 0x2e: e.in.seg = CS; continue;
        case 0x36: e.in.seg = SS; continue;
        case 0x3e: e.in.seg = DS; continue;
        case 0xf0: continue;
        case 0xf2: e.in.rep = REPNE; continue;
        case 0xf3: e.in.rep = REPE; continue;
        case 0x64: e.in.rep = REPNC; continue;
        case 0x65: e.in.rep = REPC; continue;
        default: e.exec(b); return e.cyc;
        }
    }
}

int V33::run(int cycles) {
    int done = 0;
    while (done < cycles) done += step();
    return done;
}
