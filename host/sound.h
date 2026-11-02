// The M92 sound board (host/sound.cpp): NEC V35 (encrypted opcodes) + YM2151 (ymfm) + Irem GA20.
// Output: mono at the YM2151's rate (3.579545 MHz / 64 = 55.93 kHz).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct State;
namespace sound {
constexpr double RATE = 3579545.0 / 64;
bool load(const std::string& rom_dir);   // a1-sl0 / a1-sh0 (V35 program), lh534k0k (GA20 samples), key table
void reset();
void state(::State& st);          // with generate() not running (the Vita parks the audio thread)
void generate(int16_t* out, int n);      // n mono samples at RATE; runs the V35 and both chips

// main board side (any thread)
void latch_w(uint8_t v);                 // port 0x00: command to the V35 (INTP1)
uint8_t reply_r();                       // port 0x08: the V35's answer
bool reply_pending();                    // drives the main PIC's IR3
void reset_w(bool run);                  // port 0xc0: V35 reset line (false = held in reset)
}
