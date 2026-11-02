// The game's high score table, kept across runs. The board has no EEPROM or NVRAM for it: the game copies defaults from ROM
// to RAM at boot and updates them there. Saved scores replace the defaults, and go back in after a save state is loaded, so a
// load never loses newer scores. All calls between frames, on the thread running the machine.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "board.h"

namespace hiscore {
constexpr uint32_t RAM_OFFSET = 0x25d4, ROM_DEFAULTS = 0x0fc31, SIZE = 80;   // 0xE25D4: 10 entries of 8 bytes (BCD score, initials, stage)
inline std::vector<uint8_t> saved;   // what the file holds
inline bool active;
inline uint8_t last[SIZE];
inline int settled = -1;             // frames since the last change, -1 = nothing to save

inline void set_saved(const std::vector<uint8_t>& file) { if (file.size() == SIZE) saved = file; }
inline void apply() {
    if (saved.size() == SIZE) std::memcpy(board::ram + RAM_OFFSET, saved.data(), SIZE);
    std::memcpy(last, board::ram + RAM_OFFSET, SIZE);
    active = true;
}
inline void after_state_load() { apply(); settled = -1; }
// True when the table changed and has settled for a second (no half-typed names): write 'saved' to the file.
inline bool frame() {
    uint8_t* t = board::ram + RAM_OFFSET;
    if (!active) {
        if (std::memcmp(t, &board::rom[ROM_DEFAULTS], SIZE) == 0) apply();   // the game has just set up its defaults
        return false;
    }
    if (std::memcmp(t, last, SIZE) != 0) { std::memcpy(last, t, SIZE); settled = 0; return false; }
    if (settled < 0 || ++settled < 60) return false;
    settled = -1;
    saved.assign(t, t + SIZE);
    return true;
}
}
