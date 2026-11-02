// Dispatcher for the recompiled V33 code (gen/v33/, tools/v33rec.py): runs translated routines, takes interrupts
// between them, and falls back to the interpreter at any CS:IP that has no translation.
#pragma once
#include "v33.h"
#include <set>

namespace rec {
struct Entry { uint32_t phys; uint16_t cs; void (*fn)(V33&); };
extern const Entry table[];
extern const unsigned table_count;

struct Stats { uint64_t translated, interpreted; };
extern Stats stats;
extern bool enabled;                 // false: interpreter only (reference runs)

void init();
extern std::set<uint32_t> learned;   // CS:IP where interpreted stretches began (cs << 16 | ip)
void print_hist();                   // NBB_INTERP_HIST=1: top interpreted addresses
int run(V33& c, int cycles, bool (*irq_pending)());   // about 'cycles' cycles; returns the cycles used
}
