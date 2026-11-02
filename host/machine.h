// The whole Ninja Baseball Bat Man machine (main CPU side) for the desktop runner and the Vita build: ROM loading, the V33
// (recompiled code + interpreter fallback) and one frame of scanline timing.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "v33.h"

namespace machine {
extern V33 cpu;
bool load_roms(const std::string& dir);   // MAME "nbbatmanu" file names (rom/ in this project)
void init();
void run_frame(void (*on_vblank)());      // 262 lines; on_vblank at line 248 (before the IRQ)
// Save states, only between run_frame calls and with sound::generate not running. load refuses a file of another
// version or size and then changes nothing.
void save_state(std::vector<uint8_t>& out);
bool load_state(const std::vector<uint8_t>& in);
}
