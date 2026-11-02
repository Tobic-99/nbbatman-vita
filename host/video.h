// M92 video (host/video.cpp): three 8x8-tile playfields (64x64 or 128x64, row scroll), 16x16 sprites, xBGR555.
#pragma once
#include <cstdint>
#include <vector>
#include "board.h"

namespace video {
constexpr int W = 320, H = 240;
extern std::vector<uint8_t> tile_rom;     // lh534k0c..g: four bit planes of 512 KB
extern std::vector<uint8_t> sprite_rom;   // lh538393..6: four bit planes of 1 MB
void init();                              // decode the graphics
// What the renderer reads, copied at vblank so another core can draw while the CPU runs on (Vita)
struct Frame {
    uint8_t vram[0x10000];
    uint16_t palette[0x2000], sprites[0x400];
    board::VideoRegs regs;                // at vblank: the screen below the last raster split
    board::Splits* splits = new board::Splits();   // raster splits: lines up to line[i] use regs[i]; swapped with the board's, not copied
};
void snapshot(Frame& f);
void render(const Frame& f, uint32_t* out);   // W x H, 0xAABBGGRR
void prepare(const Frame& f);                  // palette for the bands below (one thread)
void render_band(uint32_t* out, int y0, int y1);   // lines y0..y1-1 of the prepared frame (any thread)
}
