// uPD71059C interrupt controller (8259A-compatible), single chip. Our own implementation of the documented 8259
// programming model: ICW1-4 initialisation, IMR (OCW1), EOI / specific EOI (OCW2), register read select (OCW3).
#include "board.h"
#include "savestate.h"

namespace pic {
static uint8_t irr, isr, imr = 0xff, base, lines;
static bool level_mode, aeoi, read_isr;
static int init_step;          // 0 = ready; 1..3 = expecting ICW2..ICW4
static bool need_icw4, single;

void state(State& st) {
    st.pod(irr); st.pod(isr); st.pod(imr); st.pod(base); st.pod(lines);
    st.pod(level_mode); st.pod(aeoi); st.pod(read_isr); st.pod(init_step); st.pod(need_icw4); st.pod(single);
}

void reset() {
    irr = isr = lines = 0; imr = 0xff; base = 0;
    level_mode = aeoi = read_isr = false;
    init_step = 0;
}

void set_ir(int n, bool level) {
    uint8_t m = 1 << n;
    if (level && (level_mode || !(lines & m))) irr |= m;   // rising edge (or high level)
    if (!level && level_mode) irr &= ~m;
    if (level) lines |= m; else lines &= ~m;
}

// highest-priority (lowest number) request not masked and above anything in service
static int pending() {
    uint8_t req = irr & ~imr;
    for (int n = 0; n < 8; n++) {
        if (isr & (1 << n)) return -1;
        if (req & (1 << n)) return n;
    }
    return -1;
}

bool int_pending() { return pending() >= 0; }

int ack() {
    int n = pending();
    if (n < 0) return base | 7;   // spurious
    irr &= ~(1 << n);
    if (!aeoi) isr |= 1 << n;
    if (level_mode && (lines & (1 << n))) irr |= 1 << n;
    return base | n;
}

uint8_t read(int a0) {
    if (a0) return imr;
    return read_isr ? isr : irr;
}

void write(int a0, uint8_t v) {
    if (!a0) {
        if (v & 0x10) {                         // ICW1
            level_mode = v & 0x08;
            single = v & 0x02;
            need_icw4 = v & 0x01;
            imr = 0; isr = 0; irr = 0;
            init_step = 1;
            return;
        }
        if (!(v & 0x08)) {                      // OCW2: EOI
            int cmd = v >> 5;
            if (cmd == 1) {                     // non-specific EOI: the highest-priority in service
                for (int n = 0; n < 8; n++) if (isr & (1 << n)) { isr &= ~(1 << n); break; }
            } else if (cmd == 3) {
                isr &= ~(1 << (v & 7));         // specific EOI
            }
            return;
        }
        if (v & 0x02) read_isr = v & 0x01;      // OCW3: register read select
        return;
    }
    switch (init_step) {
    case 1:                                     // ICW2: vector base
        base = v & 0xf8;
        init_step = single ? (need_icw4 ? 3 : 0) : 2;
        return;
    case 2:                                     // ICW3 (cascade; unused here)
        init_step = need_icw4 ? 3 : 0;
        return;
    case 3:                                     // ICW4
        aeoi = v & 0x02;
        init_step = 0;
        return;
    }
    imr = v;                                    // OCW1
}
}
