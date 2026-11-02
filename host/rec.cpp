// Dispatcher for the recompiled V33 code. A translated routine runs until control leaves it (call, return, jump
// out, I/O, an instruction that changed CS:IP) or its slice of cycles is used; in between, the dispatcher takes
// pending interrupts exactly like the interpreter's step() does.
#include "rec.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace rec {
Stats stats;
bool enabled = true;
static std::vector<const Entry*> by_phys;
static const bool hist_on = std::getenv("NBB_INTERP_HIST") != nullptr;   // where the interpreter runs
static std::map<uint32_t, uint64_t> hist;
std::set<uint32_t> learned;   // CS:IP where an interpreted stretch began: entries for the next translation
static bool last_interp;
void print_hist() {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (auto& e : hist) v.push_back({e.second, e.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 25; i++)
        std::printf("  interpreted %04x:%04x  %llu\n", v[i].second >> 16, v[i].second & 0xffff, (unsigned long long)v[i].first);
}   // 1 MB of physical addresses -> routine (or none)

void init() {
    by_phys.assign(0x100000, nullptr);
    for (unsigned i = 0; i < table_count; i++) by_phys[table[i].phys] = &table[i];
}

int run(V33& c, int cycles, bool (*irq_pending)()) {
    int used = 0;
    while (used < cycles) {
        c.irq_line = irq_pending();
        if (c.nmi_pending || c.pending_int >= 0 || (c.IF && c.irq_line) || c.halted || !enabled) {
            last_interp = false;           // an interrupt entry: the handler start is a recorded target already
            used += c.step();              // interrupt entry / halt / interpreter-only mode
            stats.interpreted++;
            continue;
        }
        const Entry* e = by_phys[c.phys(V33::CS, c.ip)];
        if (e && e->cs == c.sreg[V33::CS]) {
            c.cycles = 0;
            c.slice = cycles - used;
            e->fn(c);
            last_interp = false;
            used += c.cycles;
            stats.translated++;
        } else {
            uint32_t key = ((uint32_t)c.sreg[V33::CS] << 16) | c.ip;
            if (hist_on) hist[key]++;
            if (!last_interp) learned.insert(key);
            last_interp = true;
            used += c.exec_one();          // no translation here: one instruction through the interpreter
            stats.interpreted++;
        }
    }
    return used;
}
}
