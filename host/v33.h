// NEC V33/V30 (x86-16) interpreter (our own implementation). Used for run-time code discovery, as the fallback
// for code the recompiler has not translated, and (with the V35 extras in sound.cpp) for the sound CPU.
// The board supplies memory (physical 20-bit addresses) and I/O through the callbacks.
#pragma once
#include <cstdint>

struct V33 {
    enum { AX, CX, DX, BX, SP, BP, SI, DI };
    enum { ES, CS, SS, DS };
    uint16_t r[8];
    uint16_t sreg[4];
    uint16_t ip;
    // flags kept unpacked
    bool CF, PF, AF, ZF, SF, OF, DF, IF, TF;
    bool halted;
    bool irq_line;                       // maskable interrupt request (vector from irq_ack)
    bool nmi_pending;
    int pending_int = -1;                // software-triggered interrupt to take before the next instruction

    // direct 4 KB pages of the 1 MB space (nullptr: go through rd8 / wr8): ROM and RAM, set by the board
    uint8_t* rpage[256] = {};
    uint8_t* wpage[256] = {};
    uint8_t (*rd8)(uint32_t addr);
    void (*wr8)(uint32_t addr, uint8_t v);
    uint8_t (*in8)(uint16_t port);
    void (*out8)(uint16_t port, uint8_t v);
    int (*irq_ack)();                    // returns the interrupt vector number
    uint8_t (*fetch8)(uint32_t addr);    // instruction fetch if set (else rpage / rd8): the V35 fetches from its ROM
                                         // even where data accesses reach the on-chip registers
    const uint8_t* optable = nullptr;    // V35 with encrypted opcodes: opcode and prefix bytes are translated
    void (*fint)() = nullptr;            // V35: 0F 92 FINT (end of interrupt in the on-chip controller)
    void (*on_call)(uint16_t cs, uint16_t ip, bool far);   // optional: every call/interrupt target (discovery)
    void (*on_jump)(uint16_t cs, uint16_t ip, bool far);   // optional: every indirect jump target (discovery)

    // recompiled code (gen/): it runs until it leaves its routine or has used 'slice' cycles, counting in 'cycles'
    int slice = 0, cycles = 0;

    void reset();
    int step();                          // one instruction (or interrupt entry); returns approximate cycles
    int exec_one();                      // exactly one instruction at CS:IP, no interrupt check (recompiled
                                         // code's fallback for rare instructions)
    int run(int cycles);
    uint16_t flags() const;
    void set_flags(uint16_t f);
    void interrupt(int vector);          // push flags/CS/IP, clear IF/TF, jump through the vector table
    uint32_t phys(int seg, uint16_t off) const { return (((uint32_t)sreg[seg] << 4) + off) & 0xFFFFF; }
};
