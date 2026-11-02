// M92 video, our own software renderer (behaviour from MAME's m92 video code as documented in the README).
//
// Playfields 0-2 (0 = top): base in video RAM = (master_control[n] & 3) * 0x4000 bytes, bit 2 = 128x64 tiles
// (else 64x64), bit 4 = off (layer 2 only checks it), bit 6 = row scroll from video RAM 0xf400 + 0x400 * n.
// Tile = 2 words: code, attr (0x8000: code bit 16, 0x200/0x400: flip x/y, 0x7f colour, 0x80/0x100: group).
// Each layer is drawn as a back half then a front half; the transparency per half depends on the tile's group.
// Priority buffer, as in MAME: a front-half pixel of any layer marks 1 and a back-half pixel writes nothing (tilemap.cpp:
// priority 0 with mask 0xff skips the write). Sprites with colour bit 7 clear (OBJ priority on) stay behind marked pixels,
// and every opaque sprite pixel, drawn or not, then marks 31 (drawgfx PRIORITY = 31). Mark 31 blocks every later sprite
// (drawgfx: "high bit of the mask is implicitly on"), so the sprite list runs front to back: the first sprite is on top.
#include "video.h"
#include "board.h"
#include <cstring>

namespace video {
std::vector<uint8_t> tile_rom, sprite_rom;
static std::vector<uint8_t> tiles, sprites;   // one byte per pixel (0-15)
static std::vector<uint16_t> tile_row_pens;   // per tile row: bit n set = pen n is used (rows no pass draws are skipped)
static std::vector<uint16_t> sprite_rows;     // per sprite cell: bit n set = row n has an opaque pixel
static int ntiles, nsprites;
constexpr int VX = 80, VY = 8;                // visible area origin in MAME's screen bitmap

void init() {
    size_t plane = tile_rom.size() / 4;
    ntiles = (int)(plane / 8);
    tiles.assign((size_t)ntiles * 64, 0);
    for (int t = 0; t < ntiles; t++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = 0;
                for (int p = 0; p < 4; p++) v |= ((tile_rom[p * plane + t * 8 + y] >> (7 - x)) & 1) << p;
                tiles[(size_t)t * 64 + y * 8 + x] = (uint8_t)v;
            }
    tile_row_pens.assign((size_t)ntiles * 8, 0);
    for (int t = 0; t < ntiles; t++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) tile_row_pens[(size_t)t * 8 + y] |= (uint16_t)(1 << tiles[(size_t)t * 64 + y * 8 + x]);
    plane = sprite_rom.size() / 4;
    nsprites = (int)(plane / 32);
    sprites.assign((size_t)nsprites * 256, 0);
    for (int t = 0; t < nsprites; t++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                size_t byte = (size_t)t * 32 + (x < 8 ? y : 16 + y);
                int v = 0;
                for (int p = 0; p < 4; p++) v |= ((sprite_rom[p * plane + byte] >> (7 - (x & 7))) & 1) << p;
                sprites[(size_t)t * 256 + y * 16 + x] = (uint8_t)v;
            }
    sprite_rows.assign(nsprites, 0);
    for (int t = 0; t < nsprites; t++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (sprites[(size_t)t * 256 + y * 16 + x]) { sprite_rows[t] |= (uint16_t)(1 << y); break; }
}

static const Frame* F;
static uint32_t lut[0x2000];
static uint8_t pri[W * H];

static uint16_t vword(const uint8_t* vr, uint32_t byte) { return (uint16_t)(vr[byte & 0xffff] | vr[(byte + 1) & 0xffff] << 8); }

// transparent-pen masks per group: [group][half] (half 0 = front, 1 = back); bit n set = pen n transparent
static uint16_t transmask(int layer, int group, int half) {
    static const uint16_t front[3] = {0xffff, 0x00ff, 0x0001};
    if (half == 0) return front[group];
    if (layer == 2) { static const uint16_t b2[3] = {0x0000, 0xff00, 0xfffe}; return b2[group]; }
    static const uint16_t b[3] = {0x0001, 0xff01, 0xffff};
    return b[group];
}

// One pass per layer: for a tile, the pens of its front half and of its back half are disjoint, so each pixel is
// drawn at most once, as front (marks priority) or back. Same result as MAME's two passes per layer.
static void draw_layer(uint32_t* out, int layer, int y0, int y1, const board::VideoRegs& R, const uint8_t* vr) {
    uint16_t mc = R.master_control[layer];
    if (mc & 0x10) return;   // playfield disabled (MAME: tmap/wide_tmap enable = ~master_control bit 4, every layer)
    bool wide = mc & 4;
    uint32_t base = (mc & 3) * 0x4000;
    int wtiles = wide ? 128 : 64, width = wtiles * 8, height = 512;
    int dx = 2 * layer - (wide ? 256 : 0), dy = -128;
    const uint16_t* ctl = R.pf_control[layer];
    bool rowscroll = R.master_control[layer] & 0x40;
    int scrolly = ctl[0], scrollx = ctl[2];
    int pal_hi = (R.videocontrol >> 7) & 0x180;
    uint16_t front[3], back[3];   // pens drawn (not transparent) per group
    for (int g = 0; g < 3; g++) { front[g] = (uint16_t)~transmask(layer, g, 0); back[g] = (uint16_t)~transmask(layer, g, 1); }
    for (int py = y0; py < y1; py++) {
        int ly = ((py + VY - dy + scrolly) % height + height) % height;
        int sx = rowscroll ? vword(vr, 0xf400 + 0x400 * layer + 2 * ly) : scrollx;
        int lx = ((VX - dx + sx) % width + width) % width;
        int trow = ly >> 3, fy = ly & 7;
        uint32_t* dst = out + py * W;
        uint8_t* pr = pri + py * W;
        for (int px = 0; px < W;) {
            int fx0 = lx & 7, n = 8 - fx0;
            if (n > W - px) n = W - px;
            uint32_t t = base + (uint32_t)(trow * wtiles + (lx >> 3)) * 4;
            uint16_t code = vword(vr, t), attr = vword(vr, t + 2);
            int tile = (code | ((attr & 0x8000) << 1)) % ntiles;   // MAME: the code wraps at the number of tiles
            int group = (attr & 0x100) ? 2 : (attr & 0x80) ? 1 : 0;
            uint16_t fm = front[group], bm = back[group];
            int flip = (attr >> 9) & 3;
            int sy = (flip & 2) ? 7 - fy : fy;
            if (tile_row_pens[(size_t)tile * 8 + sy] & (fm | bm)) {
                const uint8_t* row = &tiles[(size_t)tile * 64 + sy * 8];
                const uint32_t* pal = &lut[(((attr & 0x7f) | pal_hi) * 16) & 0x1fff];
                int step = (flip & 1) ? -1 : 1;
                const uint8_t* p = row + ((flip & 1) ? 7 - fx0 : fx0);
                if (bm == 0xffff && !fm) {          // solid and behind (the bottom layer's usual case)
                    for (int k = 0; k < n; k++, p += step) dst[px + k] = pal[*p];
                } else {
                    for (int k = 0; k < n; k++, p += step) {
                        uint16_t bit = (uint16_t)(1 << *p);
                        if (fm & bit) { dst[px + k] = pal[*p]; pr[px + k] = 1; }
                        else if (bm & bit) dst[px + k] = pal[*p];
                    }
                }
            }
            px += n;
            lx += n;
            if (lx >= width) lx -= width;
        }
    }
}

static void draw_cell(uint32_t* out, int code, int color, bool fx, bool fy, int x, int y, int pmask, int y0, int y1) {
    if (nsprites == 0) return;
    code %= nsprites;
    uint32_t rows = sprite_rows[code];
    if (!rows) return;
    const uint8_t* cell = &sprites[(size_t)code * 256];
    const uint32_t* pal = &lut[(color * 16) & 0x1fff];
    int sx = x - VX, xa = sx < 0 ? -sx : 0, xb = sx + 16 > W ? W - sx : 16;
    if (xa >= xb) return;
    uint32_t block = (uint32_t)pmask | 0x80000000u;
    for (int yy = 0; yy < 16; yy++) {
        int py = y + yy - VY;
        if (py < y0 || py >= y1) continue;
        int cy = fy ? 15 - yy : yy;
        if (!((rows >> cy) & 1)) continue;
        const uint8_t* src = cell + cy * 16;
        uint32_t* d = out + py * W + sx;
        uint8_t* pr = pri + py * W + sx;
        for (int xx = xa; xx < xb; xx++) {
            int pen = src[fx ? 15 - xx : xx];
            if (!pen) continue;
            if (!((1u << pr[xx]) & block)) d[xx] = pal[pen];
            pr[xx] = 31;
        }
    }
}

static void draw_sprites(uint32_t* out, int y0, int y1, const board::VideoRegs& R) {
    const uint16_t* s = F->sprites;
    bool pri_on = R.videocontrol & 0x2000;
    int cmask = pri_on ? 0x7f : 0xff, chigh = (R.videocontrol & 0x8000) ? 0x100 : 0;
    for (int offs = 0; offs < 0x400;) {
        int x = s[offs + 3] & 0x1ff, y = s[offs] & 0x1ff, code = s[offs + 1];
        int color = (s[offs + 2] & cmask) | chigh;
        int pmask = pri_on ? ((~s[offs + 2] >> 6) & 2) : 2;
        bool fx = (s[offs + 2] >> 8) & 1, fy = (s[offs + 2] >> 9) & 1;
        int cols = 1 << ((s[offs] >> 11) & 3), rows = 1 << ((s[offs] >> 9) & 3);
        offs += 4 * cols;
        x = (x - 16) & 0x1ff;
        y = 384 - 16 - y;
        if (fx) x += 16 * (cols - 1);
        for (int c = 0; c < cols; c++) {
            int sp = 8 * c + (fy ? 0 : rows - 1);
            for (int r = 0; r < rows; r++) {
                draw_cell(out, code + sp, color, fx, fy, x, y - r * 16, pmask, y0, y1);
                draw_cell(out, code + sp, color, fx, fy, x - 512, y - r * 16, pmask, y0, y1);
                sp += fy ? 1 : -1;
            }
            x += fx ? -16 : 16;
        }
    }
}

void snapshot(Frame& f) {
    std::memcpy(f.vram, board::vram, sizeof f.vram);
    std::memcpy(f.palette, board::palette, sizeof f.palette);
    std::memcpy(f.sprites, board::spriteram_buffer, sizeof f.sprites);
    std::memcpy(f.regs.pf_control, board::pf_control, sizeof f.regs.pf_control);
    std::memcpy(f.regs.master_control, board::master_control, sizeof f.regs.master_control);
    f.regs.videocontrol = board::videocontrol;
    board::swap_splits(f.splits);   // the finished frame's splits: no copy
}

void prepare(const Frame& frame) {
    F = &frame;
    for (int i = 0; i < 0x2000; i++) {
        uint16_t v = F->palette[i];
        uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
        r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
        lut[i] = 0xff000000u | b << 16 | g << 8 | r;
    }
}

// Lines y0..y1-1 only: disjoint bands of the same prepared frame can be drawn on different threads at once.
void render_band(uint32_t* out, int y0, int y1) {
    for (int i = y0 * W; i < y1 * W; i++) out[i] = lut[0];
    std::memset(pri + y0 * W, 0, (size_t)(y1 - y0) * W);
    // regions of rows with one register state: up to each raster split, then the vblank state
    int top = 0;
    for (int i = 0; i <= F->splits->n; i++) {
        bool last = i == F->splits->n;
        const board::VideoRegs& R = last ? F->regs : F->splits->regs[i];
        const uint8_t* vr = last ? F->vram : F->splits->vram[i];
        int bottom = last ? H : F->splits->line[i] - VY + 1;   // the split line itself still uses the old state
        int a = top > y0 ? top : y0, b = bottom < y1 ? bottom : y1;
        if (a < b) {
            for (int layer = 2; layer >= 0; layer--) draw_layer(out, layer, a, b, R, vr);
            if (!(R.videocontrol & 0x80)) draw_sprites(out, a, b, R);
        }
        if (bottom > top) top = bottom;
    }
}

void render(const Frame& frame, uint32_t* out) {
    prepare(frame);
    render_band(out, 0, H);
}
}
