// Ninja Baseball Bat Man machine: ROMs, CPU wiring and the per-scanline frame loop (6.667 MHz pixel clock, 424 x 262 per frame
// = 60.05 Hz; the 9 MHz V33 gets 572.4 cycles per line; IR0 at the first vblank line 248, IR2 at the raster line).
#include "machine.h"
#include "board.h"
#include "rec.h"
#include "video.h"
#include "sound.h"
#include "savestate.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace machine {
V33 cpu;
constexpr int LINES = 262, VBLANK_LINE = 248;
constexpr double CYCLES_PER_LINE = 9000000.0 * 424 / (26666666.0 / 4);
// Our per-instruction costs are a flat estimate (v33rec.py cycles()), too high against MAME's V33 table. Measured against MAME's
// sound commands in 3000 frames of attract (this game: all 105 match; boot alone would fit about 1.35).
constexpr double CYCLE_CALIBRATION = 1.28;
double dma_lines_left = -1;   // DMA transfer still running: lines left (board.cpp starts it)
static double owed;

static bool load(const std::string& path, std::vector<uint8_t>& v) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("missing %s\n", path.c_str()); return false; }
    std::fseek(f, 0, SEEK_END); v.resize((size_t)std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(v.data(), 1, v.size(), f) == v.size();
    std::fclose(f);
    return ok;
}

bool load_roms(const std::string& dir) {
    auto L = [&](const char* n, std::vector<uint8_t>& v) { return load(dir + "/" + n, v); };
    std::vector<uint8_t> l0, h0, l1, h1;
    if (!L("a1-l0-a.31", l0) || !L("a1-h0-a.34", h0) || !L("a1-l1-.32", l1) || !L("a1-h1-.33", h1)) return false;
    board::rom.assign(0x100000, 0xff);
    for (size_t i = 0; i < l0.size(); i++) { board::rom[2 * i] = l0[i]; board::rom[2 * i + 1] = h0[i]; }
    for (size_t i = 0; i < l1.size(); i++) { board::rom[0x80000 + 2 * i] = l1[i]; board::rom[0x80000 + 2 * i + 1] = h1[i]; }
    // graphics: tiles lh534k0c..g and sprites lh538393..6, each file one bit plane (bit 0 first)
    const char* tn[4] = {"lh534k0c.9", "lh534k0e.10", "lh534k0f.11", "lh534k0g.12"};
    const char* sn[4] = {"lh538393.42", "lh538394.43", "lh538395.44", "lh538396.45"};
    video::tile_rom.clear();
    video::sprite_rom.clear();
    if (!sound::load(dir)) return false;
    for (int i = 0; i < 4; i++) {
        std::vector<uint8_t> v;
        if (!L(tn[i], v)) return false;
        video::tile_rom.insert(video::tile_rom.end(), v.begin(), v.end());
        if (!L(sn[i], v)) return false;
        video::sprite_rom.insert(video::sprite_rom.end(), v.begin(), v.end());
    }
    return true;
}

void init() {
    board::init();
    video::init();
    cpu.rd8 = board::rd8; cpu.wr8 = board::wr8; cpu.in8 = board::in8; cpu.out8 = board::out8;
    cpu.fetch8 = nullptr;
    cpu.irq_ack = pic::ack;
    cpu.reset();
    board::map_pages(cpu);
    rec::init();
    sound::reset();
}

static constexpr uint32_t STATE_MAGIC = 0x5342424e, STATE_VERSION = 1;   // "NBBS"

static void machine_state(State& st) {
    cpu_state(st, cpu);
    st.pod(owed); st.pod(dma_lines_left);
    board::state(st);
    pic::state(st);
    sound::state(st);
}

void save_state(std::vector<uint8_t>& out) {
    out.clear();
    State st; st.out = &out;
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION;
    st.pod(magic); st.pod(version);
    machine_state(st);
}

bool load_state(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> now;
    save_state(now);
    if (in.size() != now.size() || std::memcmp(in.data(), now.data(), 8) != 0) return false;
    State st; st.in = in.data() + 8; st.left = in.size() - 8;
    machine_state(st);
    board::map_pages(cpu);
    return st.ok;
}

void run_frame(void (*on_vblank)()) {
    for (int line = 0; line < LINES; line++) {
        if (line == VBLANK_LINE) { board::latch_splits(); if (on_vblank) on_vblank(); }
        pic::set_ir(0, line == VBLANK_LINE);
        if (line == board::raster_line && line >= 8 && line < VBLANK_LINE) board::raster_split(line);
        pic::set_ir(2, line == board::raster_line);
        pic::set_ir(3, sound::reply_pending());   // the V35's answer waiting in its latch
        if (dma_lines_left >= 0 && (dma_lines_left -= 1) < 0) board::dma_finished();
        owed += CYCLES_PER_LINE * CYCLE_CALIBRATION;
        if (owed > 0) owed -= rec::run(cpu, (int)owed + 1, pic::int_pending);
    }
}
}

void sched_dma_after(int pixel_clocks) { machine::dma_lines_left = pixel_clocks / 424.0; }
