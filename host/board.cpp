// M92 main board, V33 side (our own implementation; behaviour from MAME's m92 driver as documented in the README).
//   00000-9ffff ROM · a0000-bffff ROM bank (0x80000 + bank * 0x20000) · c0000-cffff ROM mirror of 00000 ·
//   d0000-dffff video RAM · e0000-effff RAM · f8000-f87ff sprite RAM (CPU bank) · f8800-f8fff palette RAM (CPU bank)
//   f9000 DMA control (word 4 = start) · f9800 video control · ffff0-fffff = ROM 0x7fff0 (reset vector)
// Ports: 00 P1/P2 (r) / sound latch (w) · 02 coins + DMA busy, DSW3 (r) / coin counters (w) · 04 DSW1/2 ·
//   06 P3/P4 · 08 sound reply · 20 ROM bank · 40/42 PIC (A0 = 0/1) · 80-9f playfield / master control · c0 sound reset
#include "board.h"
#include "v33.h"
#include "sound.h"
#include "savestate.h"
#include <cstdio>
#include <cstring>
#include <utility>

namespace board {
std::vector<uint8_t> rom;
uint8_t ram[0x10000], vram[0x10000];
uint16_t spriteram[0x800], spriteram_buffer[0x400], paletteram[0x1000], palette[0x2000];
uint16_t dmacontrol[8], videocontrol;
uint16_t pf_control[3][4], master_control[4];
int bank, raster_line = -1;
static Splits split_buf[2];
static Splits* frame_splits = &split_buf[1];
static Splits* cur_splits = &split_buf[0];
static void read_regs(VideoRegs& r) {
    std::memcpy(r.pf_control, pf_control, sizeof r.pf_control);
    std::memcpy(r.master_control, master_control, sizeof r.master_control);
    r.videocontrol = videocontrol;
}
void raster_split(int line) {
    Splits& s = *cur_splits;
    if (s.n < MAX_SPLITS) { s.line[s.n] = line; read_regs(s.regs[s.n]); std::memcpy(s.vram[s.n], vram, sizeof vram); s.n++; }
}
void latch_splits() {   // swap: the finished frame's splits become readable, the other buffer collects the next
    Splits* done = cur_splits;
    cur_splits = frame_splits;
    cur_splits->n = 0;
    frame_splits = done;
}
void swap_splits(Splits*& other) { std::swap(frame_splits, other); }
bool dma_busy = true;
uint16_t in_p1p2 = 0xffff, in_coins = 0xff7f, in_dsw = 0xff9f;   // bit 7 of the coin port = DMA busy (active high); DSW = MAME's defaults for this game (demo sounds on, any button to start)
uint8_t sound_latch, sound_reply;
bool sound_reset = true;
static uint32_t dma_done_at;              // scheduler time (lines) when the running DMA finishes; 0 = none

void init() {
    std::memset(ram, 0, sizeof ram);
    std::memset(vram, 0, sizeof vram);
    pic::reset();
}

void state(State& st) {
    st.raw(ram, sizeof ram); st.raw(vram, sizeof vram);
    st.raw(spriteram, sizeof spriteram); st.raw(spriteram_buffer, sizeof spriteram_buffer);
    st.raw(paletteram, sizeof paletteram); st.raw(palette, sizeof palette);
    st.raw(dmacontrol, sizeof dmacontrol); st.pod(videocontrol);
    st.raw(pf_control, sizeof pf_control); st.raw(master_control, sizeof master_control);
    st.pod(bank); st.pod(raster_line); st.pod(dma_busy);
    st.pod(sound_latch); st.pod(sound_reply); st.pod(sound_reset);
    // Taken between frames, where no raster split is being collected; the last finished frame's splits are only for the screen.
    if (!st.out) cur_splits->n = 0;
}

static V33* mapped_cpu;
void map_pages(V33& c) {
    mapped_cpu = &c;
    for (int p = 0; p < 256; p++) { c.rpage[p] = nullptr; c.wpage[p] = nullptr; }
    for (int p = 0x00; p < 0xa0; p++) c.rpage[p] = &rom[p << 12];
    for (int p = 0xa0; p < 0xc0; p++) c.rpage[p] = &rom[(0x80000 + bank * 0x20000 + ((p - 0xa0) << 12)) % rom.size()];
    for (int p = 0xc0; p < 0xd0; p++) c.rpage[p] = &rom[(p - 0xc0) << 12];
    for (int p = 0xd0; p < 0xe0; p++) c.rpage[p] = c.wpage[p] = &vram[(p - 0xd0) << 12];
    for (int p = 0xe0; p < 0xf0; p++) c.rpage[p] = c.wpage[p] = &ram[(p - 0xe0) << 12];
    // 0xf0000-0xfffff: sprite/palette/DMA/video registers and the reset vector stay on the callbacks
}

static void sprite_dma(int amount) {
    const uint16_t* src = &spriteram[(videocontrol << 7) & 0x400];
    bool sort = dmacontrol[2] & 1, skip7 = !(dmacontrol[2] & 8);
    int dest = 0;
    for (int layer = 0; layer < 8; layer++) {
        for (int offs = 0, cols = 0; offs < 0x400; offs += 4 * cols) {
            int cur = (src[offs] >> 13) & 7;
            cols = 1 << ((src[offs] >> 11) & 3);
            if ((!sort || layer == cur) && !(cur == 7 && skip7)) {
                for (int i = 0; i < 4; i++) spriteram_buffer[dest + i] = src[offs + i];
                dest += 4 * cols;
                if (dest >= amount) return;
            }
        }
        if (layer == 6 && skip7) return;
    }
}

// DMA start: palette bank copy + sprite list copy; done (IR1) after its transfer time
static void dma_start() {
    int src_pal = (videocontrol << 6) & 0xc00, dst_pal = (dmacontrol[2] << 2) & 0x1c00;
    for (int i = 0; i < 0x400; i++) palette[i | dst_pal] = paletteram[i | src_pal];
    std::memset(spriteram_buffer, 0, sizeof spriteram_buffer);
    int clocks = 0x400;
    if (dmacontrol[0] & 0xff) {
        int amount = (0x100 - (dmacontrol[0] & 0xff)) * 4;
        clocks += amount;
        sprite_dma(amount);
    }
    dma_busy = false;
    pic::set_ir(1, false);
    sched_dma_after(clocks / 2);   // MAME: pixel_period / 2 * clocks (a word every half pixel clock)
}

void dma_finished() { dma_busy = true; pic::set_ir(1, true); }

static uint16_t* word16(uint32_t a, bool& ok) {
    ok = true;
    if (a >= 0xf8000 && a < 0xf8800) {
        int o = (a - 0xf8000) >> 1;
        return (dmacontrol[1] & 1) ? &spriteram_buffer[o & 0x3ff] : &spriteram[o | ((videocontrol << 10) & 0x400)];
    }
    if (a >= 0xf8800 && a < 0xf9000) {
        int o = (a - 0xf8800) >> 1;
        return (dmacontrol[1] & 2) ? &palette[o | ((videocontrol << 2) & 0x1c00)] : &paletteram[o | ((videocontrol << 9) & 0xc00)];
    }
    ok = false;
    return nullptr;
}

uint8_t rd8(uint32_t a) {
    a &= 0xfffff;
    if (a < 0xa0000) return rom[a];
    if (a < 0xc0000) return rom[(0x80000 + bank * 0x20000 + (a - 0xa0000)) % rom.size()];
    if (a < 0xd0000) return rom[a - 0xc0000];
    if (a < 0xe0000) return vram[a - 0xd0000];
    if (a < 0xf0000) return ram[a - 0xe0000];
    if (a >= 0xffff0) return rom[0x7fff0 + (a - 0xffff0)];
    bool ok;
    if (uint16_t* w = word16(a & ~1u, ok)) return (a & 1) ? *w >> 8 : *w & 0xff;
    return 0xff;
}

void wr8(uint32_t a, uint8_t v) {
    a &= 0xfffff;
    if (a >= 0xd0000 && a < 0xe0000) { vram[a - 0xd0000] = v; return; }
    if (a >= 0xe0000 && a < 0xf0000) { ram[a - 0xe0000] = v; return; }
    bool ok;
    if (uint16_t* w = word16(a & ~1u, ok)) {
        *w = (a & 1) ? (uint16_t)((*w & 0x00ff) | (v << 8)) : (uint16_t)((*w & 0xff00) | v);
        return;
    }
    if (a >= 0xf9000 && a < 0xf9010) {
        int o = (a - 0xf9000) >> 1;
        dmacontrol[o] = (a & 1) ? (uint16_t)((dmacontrol[o] & 0xff) | (v << 8)) : (uint16_t)((dmacontrol[o] & 0xff00) | v);
        if (o == 4 && !(a & 1)) dma_start();   // the value written does not matter
        return;
    }
    if (a == 0xf9800) { videocontrol = (videocontrol & 0xff00) | v; return; }
    if (a == 0xf9801) { videocontrol = (videocontrol & 0x00ff) | (v << 8); return; }
}

uint8_t in8(uint16_t port) {
    switch (port & 0xff) {
    case 0x00: return in_p1p2 & 0xff;
    case 0x01: return in_p1p2 >> 8;
    case 0x02: return (in_coins & 0x7f) | (dma_busy ? 0x80 : 0);
    case 0x03: return in_coins >> 8;
    case 0x04: return in_dsw & 0xff;
    case 0x05: return in_dsw >> 8;
    case 0x06: case 0x07: return 0xff;
    case 0x08: return sound::reply_r();        // reading clears the reply (and so IR3)
    case 0x40: return pic::read(0);
    case 0x42: return pic::read(1);
    }
    return 0xff;
}

void out8(uint16_t port, uint8_t v) {
    port &= 0xff;
    if (port >= 0x80 && port < 0x98) {           // playfield control: 3 layers x 4 words
        int l = (port - 0x80) >> 3, w = ((port - 0x80) >> 1) & 3;
        uint16_t& r = pf_control[l][w];
        r = (port & 1) ? (uint16_t)((r & 0xff) | (v << 8)) : (uint16_t)((r & 0xff00) | v);
        return;
    }
    if (port >= 0x98 && port < 0xa0) {           // master control
        int w = (port - 0x98) >> 1;
        uint16_t& r = master_control[w];
        r = (port & 1) ? (uint16_t)((r & 0xff) | (v << 8)) : (uint16_t)((r & 0xff00) | v);
        if (w == 3) { raster_line = (r & 0x1ff) - 128; pic::set_ir(2, false); }
        return;
    }
    switch (port) {
    case 0x00: sound::latch_w(v); return;        // command to the V35 (its INTP1)
    case 0x02: return;                           // coin counters
    case 0x20: bank = (v & 6) >> 1; if (mapped_cpu) map_pages(*mapped_cpu); return;
    case 0x40: pic::write(0, v); return;
    case 0x42: pic::write(1, v); return;
    case 0xc0: sound::reset_w(v != 0); return;  // V35 reset line: 0 holds it in reset
    }
}
}
