#include <cstring>
#include <cstddef>
// Desktop runner (first stage: the V33 interpreter on the board model).
//   nbb_desktop [frames]       NBB_ROOT=project dir (default .)
// Reports where the CPU spends its time every 60 frames and records every call / interrupt / indirect-jump target
// it reaches (tools/v33_runtime_targets.txt) for the recompiler's discovery.
#include "board.h"
#include "hiscore.h"
#include "v33.h"
#include "video.h"
#include "png.h"
#include "rec.h"
#include "machine.h"
#include "sound.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <vector>

static std::string root() { const char* r = std::getenv("NBB_ROOT"); return r ? r : "."; }

static V33& cpu = machine::cpu;
static std::map<uint32_t, int> hist;   // where the CPU is at vblank
static std::set<uint32_t> targets;
static void on_target(uint16_t cs, uint16_t ip, bool) { targets.insert(((uint32_t)cs << 16) | ip); }

// NBB_INPUT="1000:coin,1060:start,1200-1400:right+a": frames (or ranges) and what is held (player 1).
static void apply_input(const char* spec, int frame) {
    board::in_p1p2 = 0xffff;
    board::in_coins = 0xff7f;
    if (!spec) return;
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string range = item.substr(0, colon), keys = item.substr(colon + 1);
        int a = std::atoi(range.c_str()), b = a + 5;
        size_t dash = range.find('-');
        if (dash != std::string::npos) b = std::atoi(range.c_str() + dash + 1);
        if (frame < a || frame > b) continue;
        size_t k = 0;
        while (k <= keys.size()) {
            size_t e = keys.find('+', k);
            if (e == std::string::npos) e = keys.size();
            std::string key = keys.substr(k, e - k);
            k = e + 1;
            if (key == "coin") board::in_coins &= ~0x04;
            else if (key == "start") board::in_coins &= ~0x01;
            else if (key == "right") board::in_p1p2 &= ~0x01;
            else if (key == "left") board::in_p1p2 &= ~0x02;
            else if (key == "down") board::in_p1p2 &= ~0x04;
            else if (key == "up") board::in_p1p2 &= ~0x08;
            else if (key == "b") board::in_p1p2 &= ~0x40;
            else if (key == "a") board::in_p1p2 &= ~0x80;
        }
    }
}

int main(int argc, char** argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 600;
    if (!machine::load_roms(root() + "/rom")) return 1;
    machine::init();
    cpu.on_call = on_target;
    cpu.on_jump = on_target;
    if (const char* dump = std::getenv("NBB_RENDER_DUMP")) {   // a dumped frame -> PNG (NBB_PNG = file)
        static video::Frame f;
        FILE* d = std::fopen(dump, "rb");
        size_t head = (offsetof(video::Frame, regs) + sizeof f.regs + 3) & ~(size_t)3;   // as the Vita build writes it
        if (!d || std::fread(&f, 1, head, d) != head || std::fread(f.splits, 1, sizeof *f.splits, d) != sizeof *f.splits) { std::printf("bad dump\n"); return 1; }
        std::fclose(d);
        static uint32_t px[video::W * video::H];
        video::render(f, px);
        write_png(std::getenv("NBB_PNG"), px, video::W, video::H);
        return 0;
    }
    rec::enabled = std::getenv("NBB_INTERP") == nullptr;   // NBB_INTERP=1: interpreter only (reference)
    auto t_start = std::chrono::steady_clock::now();

    const char* pngdir = std::getenv("NBB_PNG");
    int png_every = std::getenv("NBB_PNG_EVERY") ? std::atoi(std::getenv("NBB_PNG_EVERY")) : 60;
    const char* input = std::getenv("NBB_INPUT");
    const char* wavpath = std::getenv("NBB_WAV");   // mono 16-bit at sound::RATE
    std::vector<int16_t> wav;
    double sound_owed = 0;
    std::vector<uint32_t> fb(video::W * video::H);
    // Save-state proof: NBB_SAVE_AT=frame:path, NBB_LOAD=path (continues at the saved frame), NBB_HASH=file (one hash per frame).
    const char* save_at = std::getenv("NBB_SAVE_AT");
    int first = 0;
    if (const char* lp = std::getenv("NBB_LOAD")) {
        FILE* d = std::fopen(lp, "rb");
        std::vector<uint8_t> st;
        int32_t head[1];
        if (!d || std::fread(head, 4, 1, d) != 1 || std::fread(&sound_owed, 8, 1, d) != 1) return 1;
        first = head[0];
        std::fseek(d, 0, SEEK_END); st.resize((size_t)std::ftell(d) - 12); std::fseek(d, 12, SEEK_SET);
        if (std::fread(st.data(), 1, st.size(), d) != st.size() || !machine::load_state(st)) { std::printf("load failed\n"); return 1; }
        std::fclose(d);
        std::printf("loaded %zu bytes, frame %d\n", st.size(), first);
    }
    const char* hs_path = std::getenv("NBB_HISCORE");
    if (hs_path) {
        std::vector<uint8_t> v(hiscore::SIZE + 1);
        if (FILE* d = std::fopen(hs_path, "rb")) { v.resize(std::fread(v.data(), 1, v.size(), d)); std::fclose(d); hiscore::set_saved(v); }
        if (first) hiscore::after_state_load();
    }
    const char* hashpath = std::getenv("NBB_HASH");
    FILE* hashf = hashpath ? std::fopen(hashpath, "w") : nullptr;
    for (int f = first; f < frames; f++) {
        if (save_at && f == std::atoi(save_at)) {
            std::vector<uint8_t> st;
            machine::save_state(st);
            FILE* d = std::fopen(std::strchr(save_at, ':') + 1, "wb");
            int32_t head[1] = {f};
            std::fwrite(head, 4, 1, d); std::fwrite(&sound_owed, 8, 1, d); std::fwrite(st.data(), 1, st.size(), d);
            std::fclose(d);
            std::printf("saved %zu bytes at frame %d\n", st.size(), f);
        }
        if (const char* rd = std::getenv("NBB_RAMDUMP")) {   // NBB_RAMDUMP=dir, NBB_RAMDUMP_EVERY=n: main RAM (0xE0000-0xEFFFF) to find game data
            static const int every = std::atoi(std::getenv("NBB_RAMDUMP_EVERY"));
            if (f && f % every == 0) {
                char name[512];
                std::snprintf(name, sizeof name, "%s/r%05d.bin", rd, f);
                if (FILE* d = std::fopen(name, "wb")) { std::fwrite(board::ram, 1, sizeof board::ram, d); std::fclose(d); }
            }
        }
        if (hs_path) {   // NBB_HISCORE=file: the high score file, as on the Vita
            if (hiscore::frame()) {
                if (FILE* d = std::fopen(hs_path, "wb")) { std::fwrite(hiscore::saved.data(), 1, hiscore::saved.size(), d); std::fclose(d); }
                std::printf("hiscore saved at frame %d\n", f);
            }
        }
        apply_input(input, f);
        // the picture is taken at vblank (line 248), before the game's vblank handler sets up the next frame
        static video::Frame frame;
        static bool want_png;
        // NBB_DUMP_EVERY=n, NBB_DUMP_DIR=dir: frames in the Vita dump format, to compare with MAME (tools/mame_inject.*)
        static const char* dumpdir = std::getenv("NBB_DUMP_DIR");
        static const int dump_every = std::getenv("NBB_DUMP_EVERY") ? std::atoi(std::getenv("NBB_DUMP_EVERY")) : 0;
        bool want_dump = dumpdir && dump_every && (f + 1) % dump_every == 0;
        want_png = (pngdir && (f + 1) % png_every == 0) || want_dump || hashf;
        machine::run_frame([] {
            hist[((uint32_t)cpu.sreg[V33::CS] << 16) | cpu.ip]++;
            if (want_png) video::snapshot(frame);
        });
        if (hashf) {
            video::render(frame, fb.data());
            uint64_t h = 14695981039346656037ull;
            for (uint32_t v : fb) h = (h ^ v) * 1099511628211ull;
            std::fprintf(hashf, "%d %016llx\n", f, (unsigned long long)h);
        }
        if (want_dump) {
            char name[512];
            std::snprintf(name, sizeof name, "%s/d%05d.bin", dumpdir, f + 1);
            FILE* d = std::fopen(name, "wb");
            std::fwrite(&frame, 1, (offsetof(video::Frame, regs) + sizeof frame.regs + 3) & ~(size_t)3, d);
            std::fwrite(frame.splits, 1, sizeof *frame.splits, d);
            std::fclose(d);
        }
        if (pngdir && (f + 1) % png_every == 0) {
            video::render(frame, fb.data());
            char name[512];
            std::snprintf(name, sizeof name, "%s/f%05d.png", pngdir, f + 1);
            write_png(name, fb.data(), video::W, video::H);
        }
        sound_owed += sound::RATE * 262 * 424 / (26666666.0 / 4);   // one frame of samples
        int ns = (int)sound_owed;
        sound_owed -= ns;
        std::vector<int16_t> buf(ns);
        sound::generate(buf.data(), ns);
        if (wavpath) wav.insert(wav.end(), buf.begin(), buf.end());
        if ((f + 1) % 60 == 0) {
            std::vector<std::pair<int, uint32_t>> top;
            for (auto& e : hist) top.push_back({e.second, e.first});
            std::sort(top.rbegin(), top.rend());
            std::printf("[frame %d] IF %d targets %zu | pcs:", f + 1, cpu.IF, targets.size());
            for (size_t k = 0; k < top.size() && k < 5; k++)
                std::printf(" %04x:%04x(%d)", top[k].second >> 16, top[k].second & 0xffff, top[k].first);
            std::printf("\n");
            hist.clear();
        }
    }
    if (wavpath) {
        if (FILE* w = std::fopen(wavpath, "wb")) {
            uint32_t data = (uint32_t)wav.size() * 2, riff = 36 + data, rate = (uint32_t)sound::RATE, brate = rate * 2, fmt = 16;
            uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
            std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w); std::fwrite("WAVEfmt ", 1, 8, w);
            std::fwrite(&fmt, 4, 1, w); std::fwrite(&pcm, 2, 1, w); std::fwrite(&ch, 2, 1, w);
            std::fwrite(&rate, 4, 1, w); std::fwrite(&brate, 4, 1, w); std::fwrite(&align, 2, 1, w);
            std::fwrite(&bits, 2, 1, w); std::fwrite("data", 1, 4, w); std::fwrite(&data, 4, 1, w);
            std::fwrite(wav.data(), 2, wav.size(), w);
            std::fclose(w);
        }
    }
    // NBB_LEARN=1: merge into the recorded targets: calls / jumps seen now, plus where the interpreter had to run
    if (std::getenv("NBB_LEARN")) {
        std::set<uint32_t> all(targets.begin(), targets.end());
        all.insert(rec::learned.begin(), rec::learned.end());
        if (FILE* in = std::fopen((root() + "/tools/v33_runtime_targets.txt").c_str(), "r")) {
            unsigned a, b;
            while (std::fscanf(in, "%x:%x", &a, &b) == 2) all.insert(a << 16 | b);
            std::fclose(in);
        }
        if (FILE* o = std::fopen((root() + "/tools/v33_runtime_targets.txt").c_str(), "w")) {
            for (uint32_t t : all) std::fprintf(o, "%04x:%04x\n", t >> 16, t & 0xffff);
            std::fclose(o);
        }
        std::printf("targets file: %zu entries (%zu learned this run)\n", all.size(), rec::learned.size());
    }
    std::printf("recorded %zu distinct call/jump targets\n", targets.size());
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("%.3f ms/frame | routines run %llu, interpreted instructions %llu\n", secs * 1000 / frames,
                (unsigned long long)rec::stats.translated, (unsigned long long)rec::stats.interpreted);
    rec::print_hist();
    return 0;
}
