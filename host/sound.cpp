// M92 sound board, our own code. The YM2151's FM synthesis comes from ymfm (Aaron Giles, BSD-3-Clause) when built
// with NBB_YMFM; without it our own model below provides the registers, timers A/B, status and IRQ (the driver's
// clock), and the FM output is silent.
//
// V35 map: 00000-1ffff ROM (instructions always come from here; FFFF0 = ROM 1FFF0) · a0000-a3fff RAM ·
// a8000-a803f GA20 registers on even bytes · a8040/a8042 YM2151 address/data (status on read) ·
// a8044 command latch (read; write = acknowledge) · a8046 reply latch (write) · IDB:F00-FFF on-chip registers
// (IDB = FFh after reset: FFF00-FFFFF for data accesses), IDB:E00-EFF on-chip RAM.
// Opcode and prefix bytes are decrypted with Irem's "Lethal Thunder" table (ref/irem_cpu.cpp).
// On-chip interrupt controller (only what the driver uses): INTP0 = YM2151 IRQ (vector 24), INTP1 = command latch
// (vector 25); edge-triggered requests, EXIC0-2 = request/mask/priority, an in-service bit per priority level,
// cleared by FINT.
#include "sound.h"
#include "v33.h"
#include "savestate.h"
#ifdef NBB_YMFM
#include "ymfm_opm.h"
#endif
#include <atomic>
#include <cstdio>
#include <cstring>

namespace sound {
static std::vector<uint8_t> rom, samples;
static uint8_t optable[256];
static uint8_t ram[0x4000], iram[0x100];
static V33 cpu;
static bool running = true;
static std::atomic<bool> run_req{true};
static std::atomic<int> cmd_pending{0};      // command latch written by the main CPU, not yet seen by the V35
static std::atomic<uint8_t> cmd{0}, reply{0};
static std::atomic<bool> reply_flag{false};

// ---- V35 on-chip interrupt controller ----
static uint8_t idb = 0xff;
static uint8_t pend, unmasked, ispr, prio_intp = 7;   // bits: 0 = INTP0, 1 = INTP1, 2 = INTP2
static uint8_t intp_state;

static void intp(int n, bool level) {
    uint8_t m = 1 << n;
    if (level && !(intp_state & m)) pend |= m;   // rising edge
    if (level) intp_state |= m; else intp_state &= ~m;
}
static void fint() {
    for (int i = 0; i < 8; i++) if (ispr & (1 << i)) { ispr &= ~(1 << i); break; }
}
static void take_interrupt() {
    uint8_t p = pend & unmasked;
    if (!p || !cpu.IF) return;
    for (int i = 0; i <= prio_intp; i++) if (ispr & (1 << i)) return;   // same or higher level in service
    int n = (p & 1) ? 0 : (p & 2) ? 1 : 2;
    pend &= ~(1 << n);
    ispr |= 1 << prio_intp;
    cpu.halted = false;
    cpu.interrupt(24 + n);
}
static uint8_t exic_r(int n) {
    uint8_t m = 1 << n;
    return ((pend & m) ? 0x80 : 0) | ((unmasked & m) ? 0 : 0x40) | (n == 0 ? prio_intp : 7);
}
static void exic_w(int n, uint8_t v) {
    uint8_t m = 1 << n;
    if (v & 0x80) pend |= m; else pend &= ~m;
    if (v & 0x40) unmasked &= ~m; else unmasked |= m;
    if (n == 0) prio_intp = v & 7;
}

// ---- GA20 (our implementation of its documented behaviour) ----
struct GA20 {
    uint8_t regs[0x20];
    struct Ch { uint32_t pos, end; int rate, counter, volume; bool play; } ch[4];
    void write(int o, uint8_t v) {
        o &= 0x1f;
        regs[o] = v;
        Ch& c = ch[o >> 3];
        switch (o & 7) {
        case 4: c.rate = v; break;
        case 5: c.volume = (v * 256) / (v + 10); break;
        case 6:   // key on/off: the start/end addresses are latched here
            if (v & 2) {
                c.play = true;
                c.pos = (regs[o & ~7] | regs[(o & ~7) + 1] << 8) << 4;
                c.end = (regs[(o & ~7) + 2] | regs[(o & ~7) + 3] << 8) << 4;
                c.counter = 0x100;
            } else c.play = false;
            break;
        }
    }
    uint8_t read(int o) { return (o & 7) == 7 ? ch[(o >> 3) & 3].play : 0; }
    // 16 chip samples at once (one output sample), exactly as 16 single ticks: within a segment the channel stays
    // on one byte; it moves on the tick where its counter reaches the rate
    int tick16() {
        int out = 0;
        for (Ch& c : ch) {
            int left = 16;
            while (c.play && left > 0) {
                int s = samples[c.pos % samples.size()];
                if (s == 0) { c.play = false; break; }   // 0 ends a sample
                int n = c.counter - c.rate;
                if (n < 1) n = 1;
                if (n > left) n = left;
                out += n * (s - 0x80) * c.volume;
                c.counter -= n;
                left -= n;
                if (c.counter <= c.rate) { c.pos++; c.counter = 0x100; }
            }
        }
        return out;
    }
} ga20;

// ---- YM2151 ----
#ifdef NBB_YMFM
struct YmHost : ymfm::ymfm_interface {
    int32_t timer_left[2] = {-1, -1};   // in YM2151 clocks
    void ymfm_set_timer(uint32_t t, int32_t clocks) override { timer_left[t] = clocks; }
    void ymfm_update_irq(bool asserted) override { intp(0, asserted); }
    void advance(int clocks) {
        for (int t = 0; t < 2; t++)
            if (timer_left[t] >= 0 && (timer_left[t] -= clocks) < 0) { timer_left[t] = -1; m_engine->engine_timer_expired(t); }
    }
};
static YmHost ymhost;
static ymfm::ym2151 ym(ymhost);
static void ym_reset() { ym.reset(); }
static uint8_t ym_status() { return ym.read_status(); }
static void ym_address(uint8_t v) { ym.write_address(v); }
static void ym_data(uint8_t v) { ym.write_data(v); }
static void ym_state(State& st) {
    st.raw(ymhost.timer_left, sizeof ymhost.timer_left);
    uint32_t n = 0;
    std::vector<uint8_t> buf;
    if (st.out) { ymfm::ymfm_saved_state s(buf, true); ym.save_restore(s); n = (uint32_t)buf.size(); }
    st.pod(n);
    if (!st.out) buf.resize(n);
    st.raw(buf.data(), n);
    if (!st.out && st.ok) { ymfm::ymfm_saved_state s(buf, false); ym.save_restore(s); }
}
static int ym_sample() {   // one sample (both outputs summed), advancing the timers by 64 clocks
    ymhost.advance(64);
    ymfm::ym2151::output_data o;
    ym.generate(&o);
    return o.data[0] + o.data[1];
}
#else
// Our model of the YM2151's control side (no FM output yet): registers, timer A (10 bit, 64 clocks per step:
// period 64 * (1024 - TA) clocks), timer B (8 bit, 1024 clocks per step), register 0x14 = load (bits 0-1), IRQ
// enable (2-3), reset flags (4-5); status bits 0/1 = timer A/B overflowed; IRQ = any enabled flag set.
struct Opm {
    uint8_t addr, regs[256];
    int32_t left[2];
    uint8_t status;
    void irq_update() { intp(0, (status & 1 && regs[0x14] & 4) || (status & 2 && regs[0x14] & 8)); }
    int32_t period(int t) const {
        return t == 0 ? 64 * (1024 - ((regs[0x10] << 2) | (regs[0x11] & 3))) : 1024 * (256 - regs[0x12]);
    }
    void write(uint8_t v) {
        uint8_t old = regs[addr];
        regs[addr] = v;
        if (addr == 0x14) {
            if (v & 0x10) status &= ~1;
            if (v & 0x20) status &= ~2;
            for (int t = 0; t < 2; t++)
                if ((v & (1 << t)) && !(old & (1 << t))) left[t] = period(t);   // starts on 0 -> 1 of its load bit
            irq_update();
        }
    }
    void advance(int clocks) {
        for (int t = 0; t < 2; t++) {
            if (!(regs[0x14] & (1 << t))) continue;
            if ((left[t] -= clocks) <= 0) {
                left[t] += period(t);
                status |= 1 << t;
                irq_update();
            }
        }
    }
} opm;
static void ym_reset() { std::memset(&opm, 0, sizeof opm); }
static void ym_state(State& st) { st.pod(opm); }
static uint8_t ym_status() { return opm.status; }
static void ym_address(uint8_t v) { opm.addr = v; }
static void ym_data(uint8_t v) { opm.write(v); }
static int ym_sample() { opm.advance(64); return 0; }
#endif

// ---- V35 memory and I/O ----
static uint8_t sfr[0x100];
static bool is_internal(uint32_t a) { return (a >> 12) == idb && (a & 0xe00) == 0xe00; }

static uint8_t rd8(uint32_t a) {
    a &= 0xfffff;
    if (is_internal(a)) {
        if ((a & 0xf00) == 0xe00) return iram[a & 0xff];
        switch (a & 0xff) {
        case 0x4c: return exic_r(0);
        case 0x4d: return exic_r(1);
        case 0x4e: return exic_r(2);
        case 0xfc: return ispr;
        case 0xff: return idb;
        }
        return sfr[a & 0xff];
    }
    if (a < 0x20000) return rom[a];
    if (a >= 0xa0000 && a < 0xa4000) return ram[a - 0xa0000];
    if (a >= 0xa8000 && a < 0xa8040) return (a & 1) ? 0 : ga20.read((a - 0xa8000) >> 1);
    if (a == 0xa8040 || a == 0xa8042) return ym_status();
    if (a == 0xa8044) return cmd.load();
    if (a >= 0xffff0) return rom[0x1fff0 + (a & 0xf)];
    return 0xff;
}
static void wr8(uint32_t a, uint8_t v) {
    a &= 0xfffff;
    if (is_internal(a)) {
        if ((a & 0xf00) == 0xe00) { iram[a & 0xff] = v; return; }
        switch (a & 0xff) {
        case 0x4c: exic_w(0, v); return;
        case 0x4d: exic_w(1, v); return;
        case 0x4e: exic_w(2, v); return;
        case 0xfc: ispr = v; return;
        case 0xff: idb = v; return;
        }
        sfr[a & 0xff] = v;
        return;
    }
    if (a >= 0xa0000 && a < 0xa4000) { ram[a - 0xa0000] = v; return; }
    if (a >= 0xa8000 && a < 0xa8040) { if (!(a & 1)) ga20.write((a - 0xa8000) >> 1, v); return; }
    if (a == 0xa8040) { ym_address(v); return; }
    if (a == 0xa8042) { ym_data(v); return; }
    if (a == 0xa8044) { intp(1, false); return; }               // acknowledge the command
    if (a == 0xa8046) { reply = v; reply_flag = true; return; }  // to the main CPU (its PIC IR3)
}
static uint8_t fetch8(uint32_t a) {   // instructions always come from the ROM side
    a &= 0xfffff;
    if (a < 0x20000) return rom[a];
    if (a >= 0xffff0) return rom[0x1fff0 + (a & 0xf)];
    if (a >= 0xa0000 && a < 0xa4000) return ram[a - 0xa0000];
    return 0xff;
}
static uint8_t in8(uint16_t) { return 0xff; }
static void out8(uint16_t, uint8_t) {}

static bool load_file(const std::string& p, std::vector<uint8_t>& v) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) { std::printf("missing %s\n", p.c_str()); return false; }
    std::fseek(f, 0, SEEK_END); v.resize((size_t)std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(v.data(), 1, v.size(), f) == v.size();
    std::fclose(f);
    return ok;
}

// Irem's opcode table for this CPU ("leagueman_decryption_table" in MAME's irem_cpu.cpp, BSD-3-Clause;
// the 0x90 entries are the unknown ones). Kept as data here.
static const uint8_t sound_key[256] = {
#include "nbb_v35_key.inc"
};

bool load(const std::string& dir) {
    std::vector<uint8_t> lo, hi;
    if (!load_file(dir + "/a1-sl0-.17", lo) || !load_file(dir + "/a1-sh0-.14", hi) || !load_file(dir + "/lh534k0k.8", samples))
        return false;
    rom.assign(0x20000, 0xff);
    for (size_t i = 0; i < lo.size(); i++) { rom[2 * i] = lo[i]; rom[2 * i + 1] = hi[i]; }
    std::memcpy(optable, sound_key, 256);
    return true;
}

void reset() {
    std::memset(ram, 0, sizeof ram);
    std::memset(iram, 0, sizeof iram);
    std::memset(sfr, 0, sizeof sfr);
    std::memset(&ga20, 0, sizeof ga20);
    idb = 0xff; pend = 0; unmasked = 0; ispr = 0; prio_intp = 7; intp_state = 0;
    ym_reset();
    cpu.rd8 = rd8; cpu.wr8 = wr8; cpu.in8 = in8; cpu.out8 = out8;
    cpu.fetch8 = fetch8;
    cpu.optable = optable;
    cpu.fint = fint;
    cpu.irq_ack = nullptr;
    cpu.reset();
    for (int p = 0; p < 0x20; p++) cpu.rpage[p] = &rom[p << 12];
    for (int p = 0xa0; p < 0xa4; p++) cpu.rpage[p] = cpu.wpage[p] = &ram[(p - 0xa0) << 12];
}

// The driver's idle loop, 96.6% of its instructions in the attract: 08E2 test byte [ss:0C34],80h (ss: f6 06 34 0c 80) / 08E8 jz 08E2.
// It waits for an interrupt handler to set that bit. Interrupts are taken at the start of a sample, so an already taken
// handler may have set it (checked here); otherwise nothing can change for the rest of the sample: all flags stay as they
// are and only the cycles pass. The same cycle count per instruction (learned from the interpreter) keeps the carry into the next sample exact.
static const uint16_t IDLE_TEST = 0x8e2, IDLE_JZ = 0x8e8;
static int idle_cost_test = -1, idle_cost_jz = -1;
static bool idle_loop(double& owed) {
    uint32_t pc = cpu.phys(V33::CS, cpu.ip);
    if (pc != IDLE_JZ || !cpu.ZF || !cpu.IF || idle_cost_test < 0 || idle_cost_jz < 0) return false;
    if (rd8(cpu.phys(V33::SS, 0xc34)) & 0x80) return false;   // set by a handler in this sample: the loop ends now
    uint32_t cs_base = pc - cpu.ip;   // the loop is addressed through any CS:IP pair
    uint32_t at = IDLE_JZ;
    while (owed > 0) {
        if (at == IDLE_JZ) { owed -= idle_cost_jz; at = IDLE_TEST; }
        else { owed -= idle_cost_test; at = IDLE_JZ; }
    }
    cpu.ip = (uint16_t)(at - cs_base);
    return true;
}

static double gen_owed;   // V35 cycles carried between samples

void state(State& st) {
    cpu_state(st, cpu);
    st.raw(ram, sizeof ram); st.raw(iram, sizeof iram); st.raw(sfr, sizeof sfr);
    st.pod(running); st.pod(idb); st.pod(pend); st.pod(unmasked); st.pod(ispr); st.pod(prio_intp); st.pod(intp_state);
    st.pod(ga20); st.pod(gen_owed);
    bool rr = run_req.load(), rf = reply_flag.load(); int cp = cmd_pending.load(); uint8_t cm = cmd.load(), re = reply.load();
    st.pod(rr); st.pod(rf); st.pod(cp); st.pod(cm); st.pod(re);
    run_req = rr; reply_flag = rf; cmd_pending = cp; cmd = cm; reply = re;
    ym_state(st);
}

void generate(int16_t* out, int n) {
    // V35: 14.318 MHz crystal, internal clock / 2 -> 7.16 MHz = 128 cycles per output sample
    constexpr double V35_PER_SAMPLE = 14318181.0 / 2 / RATE;
    double& owed = gen_owed;
    for (int i = 0; i < n; i++) {
        bool want = run_req.load();
        if (want && !running) cpu.reset(), running = true;
        if (!want) running = false;
        if (int c = cmd_pending.exchange(0)) { (void)c; intp(1, false); intp(1, true); }   // new command: INTP1 edge
        owed += V35_PER_SAMPLE;
        while (running && owed > 0) {
            take_interrupt();
            if (idle_loop(owed)) break;
            uint32_t pc = cpu.phys(V33::CS, cpu.ip);
            uint16_t ip0 = cpu.ip;
            int c = cpu.halted ? (int)owed : cpu.exec_one();
            if (pc == IDLE_TEST) idle_cost_test = c;
            else if (pc == IDLE_JZ && cpu.ip != (uint16_t)(ip0 + 2)) idle_cost_jz = c;   // learn the interpreter's costs
            owed -= c;
        }
        if (!running) owed = 0;
        int fm = ym_sample();
        int g = ga20.tick16();   // 3.58 MHz / 4 = 16 GA20 samples per YM2151 sample
        double mix = fm * 0.40 / 32768.0 + (g / 16.0) / (32768.0 * 2);   // MAME: both GA20 outputs reach the mono speaker at 1.0
        if (mix > 1) mix = 1; else if (mix < -1) mix = -1;
        out[i] = (int16_t)(mix * 32767);
    }
}

void latch_w(uint8_t v) { cmd = v; cmd_pending = 1; }
uint8_t reply_r() { reply_flag = false; return reply.load(); }
bool reply_pending() { return reply_flag.load(); }
void reset_w(bool run) { run_req = run; }
}
