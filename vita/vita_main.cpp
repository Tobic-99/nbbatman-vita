// Ninja Baseball Bat Man - PS Vita platform layer: boot, ROM loading, input, frame pacing, presentation.
// The ROMs are NOT part of the VPK; they are read from ux0:data/nbbatman/rom/ (MAME "nbbatmanu" file names).
//
// Threads:
//   emu    (core 0, priority 0x60): the machine (recompiled V33 + interpreter fallback) one frame at a time; at
//          every vblank it copies the video state into a free Frame slot and publishes it. Paced to 60.05 Hz.
//   render (core 1, priority 0x48): owns vitaGL; draws the newest Frame in software into a texture, presents it.
//   statewriter (core 1, priority 0xa0): writes save states to the memory card.
//   audio  (core 2, priority 0x40): runs the sound board (V35 + YM2151 + GA20, host/sound.cpp) as fast as
//          sceAudioOut takes samples: 55.93 kHz resampled to 48 kHz. The main CPU reaches it only through atomics.
#include "board.h"
#include "hiscore.h"
#include "machine.h"
#include "rec.h"
#include "sound.h"
#include <psp2/audioout.h>
#include "video.h"
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>
#include "png.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int _newlib_heap_size_user = 160 * 1024 * 1024;   // ROMs ~10 MB + decoded graphics ~20 MB; vitaGL the rest

// vita-elf-create needs ~3.4 KB free at the end of the text segment, whose size depends on the build modulo 64 KB ("segment 1
// overlaps"). This pad moves the end of the text past a boundary; resize it if that error returns.
extern const char elf_pad[] __attribute__((used));
const char elf_pad[0x2000] __attribute__((used, section(".rodata"))) = {1};

// Profile builds run from the PC with nothing on the memory card: ROMs, log and dumps on host0: (tools: see the README).
#ifdef NBB_DUMP
static const std::string BASE = "host0:";
#else
static const std::string BASE = "ux0:data/nbbatman";
#endif

[[noreturn]] static void fatal(const char* what) {
    std::printf("[vita] fatal: %s\n", what);
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitProcess(1);
    for (;;) {}
}

static video::Frame frames[3];
static uint32_t frame_no[3];                    // guest frame number of each slot
static constexpr uint32_t BOOT_BLANK_FRAMES = 460;   // the power-on RAM test fills video RAM with patterns (frames 350-455, shown black)
static std::atomic<int> newest{-1}, drawing{-1};
static std::atomic<uint32_t> published{0};
static std::atomic<uint64_t> emu_us_acc{0}, emu_frames{0}, audio_us_acc{0};
static std::atomic<bool> sound_ready{false};   // the audio thread may run the sound board

// ---- save states: ux0:data/nbbatman/state.bin (about 180 KB) ----
// SELECT + L saves, SELECT + R loads, SELECT + START held 2 s deletes the file. Also saved every 3 minutes of play. A state
// found at launch resumes; hold SELECT at launch to start fresh. Taken between frames with the audio thread parked; a
// thread writes the file (to .tmp, then renamed), so the game keeps running. A green / blue / red bar in the corner of
// the screen = saved / loaded / failed.
static const char* const STATE_PATH = "ux0:data/nbbatman/state.bin";
static const std::string BASE_DATA = "ux0:data/nbbatman";
static std::atomic<bool> want_save{false}, want_load{false}, state_writing{false};
static std::atomic<int> audio_park{0};      // 0 = run, 1 = asked to stop, 2 = stopped between grains
static std::atomic<uint32_t> state_bar{0};  // colour of the corner bar
static std::atomic<int> state_bar_frames{0};
static SceUID state_sem;
static std::vector<uint8_t> state_buf;
static std::atomic<bool> hiscore_pending{false};
static std::vector<uint8_t> hiscore_buf;
static void show_state_bar(uint32_t abgr) { state_bar = abgr; state_bar_frames = 60; }
static void park_audio() {
    if (!sound_ready) return;
    audio_park = 1;
    while (audio_park != 2) sceKernelDelayThread(500);
}
static void unpark_audio() { audio_park = 0; }

static int state_writer_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(state_sem, 1, nullptr);
        if (hiscore_pending) {
            std::string hs = BASE_DATA + "/hiscores.bin", tmp = hs + ".tmp";
            bool ok = false;
            if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
                ok = std::fwrite(hiscore_buf.data(), 1, hiscore_buf.size(), f) == hiscore_buf.size();
                ok = std::fclose(f) == 0 && ok;
                if (ok) { sceIoRemove(hs.c_str()); ok = sceIoRename(tmp.c_str(), hs.c_str()) >= 0; }
            }
            std::printf("[state] high scores saved: %s\n", ok ? "ok" : "FAILED");
            hiscore_pending = false;
        }
        if (!state_writing) continue;
        std::string tmp = std::string(STATE_PATH) + ".tmp";
        uint64_t t0 = sceKernelGetProcessTimeWide();
        bool ok = false;
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            ok = std::fwrite(state_buf.data(), 1, state_buf.size(), f) == state_buf.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok) {   // sce calls: their errors are reported, and the old state is only removed right before the rename
                sceIoRemove(STATE_PATH);
                int r = sceIoRename(tmp.c_str(), STATE_PATH);
                if (r < 0) std::printf("[state] rename failed %08x\n", r);
                ok = r >= 0;
            }
        }
        std::printf("[state] wrote %u bytes in %.1f ms: %s\n", (unsigned)state_buf.size(), (sceKernelGetProcessTimeWide() - t0) / 1e3, ok ? "ok" : "FAILED");
        show_state_bar(ok ? 0xff00d000u : 0xff0000ffu);
        state_writing = false;
    }
}

static bool read_state_file(std::vector<uint8_t>& out) {
    FILE* f = std::fopen(STATE_PATH, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); out.resize((size_t)std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// ---- input ----
// D-pad / left stick move; Cross = shoot (button 1); Circle = button 2.
// START = coin + 1P start (one press: a coin, then start); SELECT (short press, alone) = pause / resume.
static std::atomic<bool> paused{false};
static bool aspect43;   // ux0:data/nbbatman/aspect43 exists: 4:3 picture instead of full screen
static void poll_input() {
    SceCtrlData c{};
    sceCtrlPeekBufferPositive(0, &c, 1);
    uint32_t b = c.buttons;
    if (c.lx < 64) b |= SCE_CTRL_LEFT;
    if (c.lx > 192) b |= SCE_CTRL_RIGHT;
    if (c.ly < 64) b |= SCE_CTRL_UP;
    if (c.ly > 192) b |= SCE_CTRL_DOWN;
    // SELECT: a short press on its own toggles pause
    static bool sel_down, sel_other;
    static uint64_t sel_t0;
    uint64_t now = sceKernelGetProcessTimeWide();
    bool sel = b & SCE_CTRL_SELECT, others = b & ~SCE_CTRL_SELECT & 0xFFFF;
    static bool sl_down, sr_down;
    static uint64_t del_t0;
    static bool del_done;
    bool sl = sel && (b & SCE_CTRL_LTRIGGER), sr = sel && (b & SCE_CTRL_RTRIGGER);
    if (sl && !sl_down) want_save = true;
    if (sr && !sr_down) want_load = true;
    sl_down = sl; sr_down = sr;
    if (sel && (b & SCE_CTRL_START)) {
        if (!del_t0) del_t0 = now;
        else if (!del_done && now - del_t0 > 2000000) {
            del_done = true;   // once per hold
            int r = sceIoRemove(STATE_PATH);
            std::printf("[state] SELECT+START: delete %08x\n", r);
            show_state_bar(0xff0000ffu);
        }
    } else { del_t0 = 0; del_done = false; }
    if (sel && !sel_down) { sel_down = true; sel_other = false; sel_t0 = now; }
    if (sel && others) sel_other = true;
    if (!sel && sel_down) {
        sel_down = false;
        if (!sel_other && now - sel_t0 < 700000) paused = !paused.load();
    }
    if (paused) return;
    uint16_t p = 0xffff;   // P1 in the low byte, active low
    if (b & SCE_CTRL_RIGHT)  p &= ~0x01;
    if (b & SCE_CTRL_LEFT)   p &= ~0x02;
    if (b & SCE_CTRL_DOWN)   p &= ~0x04;
    if (b & SCE_CTRL_UP)     p &= ~0x08;
    if (b & SCE_CTRL_CIRCLE) p &= ~0x40;
    if (b & SCE_CTRL_CROSS)  p &= ~0x80;
    board::in_p1p2 = p;
    // START press: coin 1 for frames 0-5, then 1P start for frames 10-15 (the game counts the coin first)
    static bool start_down;
    static int start_seq = -1;
    bool st = (b & SCE_CTRL_START) && !sel;
    if (st && !start_down) start_seq = 0;
    start_down = st;
    uint16_t coins = 0xff7f;
    if (start_seq >= 0) {
        if (start_seq < 6) coins &= ~0x04;
        else if (start_seq >= 10 && start_seq < 16) coins &= ~0x01;
        if (++start_seq >= 16) start_seq = -1;
    }
    board::in_coins = coins;
}

static uint32_t frames_seen;
static void on_vblank() {
    int slot = 0;
    while (slot == newest.load() || slot == drawing.load()) slot++;
    video::snapshot(frames[slot]);
    frame_no[slot] = ++frames_seen;
    newest.store(slot);
    published++;
    poll_input();
    if ((frames_seen & 31) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // keep the screen on
}

// Between frames only. Returns true when a state was loaded (the caller rebases its pacing).
static bool handle_state_requests() {
    if (hiscore::frame() && !hiscore_pending) {
        hiscore_buf = hiscore::saved;
        hiscore_pending = true;
        sceKernelSignalSema(state_sem, 1);
    }
    static uint32_t last_auto;
    if (frames_seen > BOOT_BLANK_FRAMES + 600 && frames_seen - last_auto >= 10800 && !state_writing) {   // 3 minutes
        last_auto = frames_seen;
        want_save = true;
        std::printf("[state] 3-minute save\n");
    }
    if (want_save.exchange(false)) {
        if (state_writing) std::printf("[state] still writing the last save\n");
        else {
            uint64_t t0 = sceKernelGetProcessTimeWide();
            park_audio();
            machine::save_state(state_buf);
            unpark_audio();
            std::printf("[state] snapshot %.2f ms\n", (sceKernelGetProcessTimeWide() - t0) / 1e3);
            state_writing = true;
            sceKernelSignalSema(state_sem, 1);
        }
    }
    if (want_load.exchange(false)) {
        while (state_writing) sceKernelDelayThread(1000);   // a save just asked for is what this load should read
        static std::vector<uint8_t> in;
        bool ok = read_state_file(in);
        if (ok) { park_audio(); ok = machine::load_state(in); if (ok) hiscore::after_state_load(); unpark_audio(); }
        std::printf("[state] load: %s\n", ok ? "ok" : "no usable state");
        show_state_bar(ok ? 0xffd08000u : 0xff0000ffu);
        if (ok) { paused = false; if (frames_seen < BOOT_BLANK_FRAMES) frames_seen = BOOT_BLANK_FRAMES; return true; }
    }
    return false;
}

#ifdef NBB_DUMP
// Every pause writes the newest frame's raw video state to host0: (tools/frame_dump.py, tools/mame_inject.*).
static void dump_frame() {
    static int n;
    char name[64];
    std::snprintf(name, sizeof name, "host0:/frame_%03d.bin", n++);
    SceUID fd = sceIoOpen(name, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { std::printf("[vita] dump %s failed %08x\n", name, fd); return; }
    const video::Frame& f = frames[newest.load()];
    int wrote = sceIoWrite(fd, &f, (offsetof(video::Frame, regs) + sizeof f.regs + 3) & ~3u);
    wrote += sceIoWrite(fd, f.splits, sizeof *f.splits);
    sceIoClose(fd);
    std::printf("[vita] dump %s: %d bytes\n", name, wrote);
}
#endif

static int emu_thread(SceSize, void*) {
    if (!machine::load_roms(BASE + "/rom")) fatal("ROM files missing in ux0:data/nbbatman/rom/ (see README)");
    std::printf("[vita] ROMs loaded\n");
    machine::init();
    {   // high scores, kept apart from save states: they survive starting fresh and deleting a state
        std::vector<uint8_t> v(hiscore::SIZE + 1);
        if (FILE* f = std::fopen((BASE_DATA + "/hiscores.bin").c_str(), "rb")) { v.resize(std::fread(v.data(), 1, v.size(), f)); std::fclose(f); hiscore::set_saved(v); }
    }
    {   // resume, unless SELECT is held
        SceCtrlData c{};
        sceCtrlPeekBufferPositive(0, &c, 1);
        std::vector<uint8_t> in;
        if (c.buttons & SCE_CTRL_SELECT) std::printf("[state] SELECT held: starting fresh\n");
        else if (read_state_file(in)) {
            bool ok = machine::load_state(in);
            std::printf("[state] resume: %s\n", ok ? "ok" : "state refused, starting fresh");
            if (ok) { hiscore::after_state_load(); frames_seen = BOOT_BLANK_FRAMES; show_state_bar(0xffd08000u); }
        }
    }
    sound_ready = true;
    const uint64_t period_us = 16651;          // 262 lines x 424 pixels at 6.667 MHz: 60.05 Hz
    uint64_t next = sceKernelGetProcessTimeWide();
    for (;;) {
        if (paused) {
            std::printf("[vita] paused\n");
#ifdef NBB_DUMP
            dump_frame();
#endif
            while (paused) { sceKernelDelayThread(16667); poll_input(); handle_state_requests(); sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }
            std::printf("[vita] resumed\n");
            next = sceKernelGetProcessTimeWide();
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        machine::run_frame(on_vblank);
        uint64_t t1 = sceKernelGetProcessTimeWide();
        emu_us_acc += t1 - t0; emu_frames++;
        if (handle_state_requests()) { next = sceKernelGetProcessTimeWide(); continue; }
        next += period_us;
        if (t1 < next) sceKernelDelayThread((SceUInt)(next - t1));
        else if (t1 - next > 4 * period_us) next = t1;   // far behind: do not try to catch up
    }
}

static int audio_thread(SceSize, void*) {
    constexpr int GRAIN = 512, OUT_HZ = 48000;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_HZ, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { std::printf("[audio] open port failed %08x\n", port); return 0; }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    static int16_t out[2][GRAIN * 2];
    static int16_t in[GRAIN * 2 + 8];
    int have = 0;           // input samples (mono, sound::RATE) not consumed yet
    double pos = 0;
    const double step = sound::RATE / OUT_HZ;
    for (int b = 0;; b ^= 1) {
        if (audio_park == 1) {
            audio_park = 2;
            while (audio_park != 0) sceKernelDelayThread(500);
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        if (paused || !sound_ready) {
            std::memset(out[b], 0, sizeof out[b]);
        } else {
            int need = (int)(pos + GRAIN * step) + 2;
            if (need > have) { sound::generate(in + have, need - have); have = need; }
            for (int i = 0; i < GRAIN; i++) {
                int k = (int)pos;
                double fr = pos - k;
                int16_t v = (int16_t)(in[k] + (in[k + 1] - in[k]) * fr);
                out[b][2 * i] = out[b][2 * i + 1] = v;
                pos += step;
            }
            int used = (int)pos;
            std::memmove(in, in + used, (have - used) * 2);
            have -= used; pos -= used;
        }
        audio_us_acc += sceKernelGetProcessTimeWide() - t0;
        sceAudioOutOutput(port, out[b]);   // blocks until the hardware takes the grain: paces the sound board
    }
}

// The frame is drawn in two bands at once: the render thread (core 1) the top, the band thread (core 0, next to the
// main CPU) the bottom; video::render_band keeps disjoint lines independent. Drawing and presenting overlap: two
// pixel buffers, and a present thread (core 0) uploads frame N-1 and waits for the display's vblank.
static constexpr int BAND_SPLIT = 160;   // top two thirds on core 1: core 0 also runs the main CPU and the upload
static SceUID band_go, band_done, buf_free, buf_ready;
static uint32_t* band_pixels;
static uint32_t pix[2][video::W * video::H];
static std::atomic<uint64_t> render_us_acc{0}, top_us_acc{0}, bottom_us_acc{0}, band_wait_us_acc{0}, upload_us_acc{0}, swap_us_acc{0};
static int band_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(band_go, 1, nullptr);
        uint64_t t0 = sceKernelGetProcessTimeWide();
        video::render_band(band_pixels, BAND_SPLIT, video::H);
        bottom_us_acc += sceKernelGetProcessTimeWide() - t0;
        sceKernelSignalSema(band_done, 1);
    }
}

static int render_thread(SceSize, void*) {
    uint32_t seen = 0;
    int wi = 0;
    for (;;) {
        if (published.load() == seen) { sceKernelDelayThread(1000); continue; }
        seen = published.load();
        sceKernelWaitSema(buf_free, 1, nullptr);
        int slot;
        do { slot = newest.load(); drawing.store(slot); } while (newest.load() != slot);   // pin it before the emu reuses it
        uint64_t t0 = sceKernelGetProcessTimeWide();
        if (frame_no[slot] < BOOT_BLANK_FRAMES) {
            std::memset(pix[wi], 0, sizeof pix[wi]);
        } else {
            video::prepare(frames[slot]);
            band_pixels = pix[wi];
            sceKernelSignalSema(band_go, 1);
            uint64_t t1 = sceKernelGetProcessTimeWide();
            video::render_band(pix[wi], 0, BAND_SPLIT);
            uint64_t t2 = sceKernelGetProcessTimeWide();
            top_us_acc += t2 - t1;
            sceKernelWaitSema(band_done, 1, nullptr);
            band_wait_us_acc += sceKernelGetProcessTimeWide() - t2;
        }
        drawing.store(-1);
        render_us_acc += sceKernelGetProcessTimeWide() - t0;
        wi ^= 1;
        sceKernelSignalSema(buf_ready, 1);
    }
}

static int present_thread(SceSize, void*) {
    vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);
    // glTexSubImage2D takes about 8 ms a frame on the Vita: copy straight into the texture's memory, two textures so the
    // GPU never reads the one being written.
    GLuint tex[2];
    uint32_t* tex_data[2];
    glGenTextures(2, tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        tex_data[i] = (uint32_t*)vglGetTexDataPointer(GL_TEXTURE_2D);
    }
    int ti = 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    uint32_t shown = 0;
    int ri = 0;
    uint64_t t_last = sceKernelGetProcessTimeWide();
    for (;;) {
        sceKernelWaitSema(buf_ready, 1, nullptr);
        uint64_t p0 = sceKernelGetProcessTimeWide();
        for (int y = 0; y < video::H; y++)
            sceClibMemcpy(tex_data[ti] + y * 512, pix[ri] + y * video::W, video::W * 4);
        if (state_bar_frames.load() > 0) {
            state_bar_frames--;
            uint32_t col = state_bar.load();
            for (int y = 4; y < 10; y++) for (int x = 4; x < 44; x++) tex_data[ti][y * 512 + x] = col;
        }
        glBindTexture(GL_TEXTURE_2D, tex[ti]);
        ti ^= 1;
        // test aid: with a file "shots" in the data folder, save every 300th frame
        static int shots = -1;
        if (shots < 0) { SceIoStat st; shots = sceIoGetstat((BASE + "/shots").c_str(), &st) >= 0; }
        if (shots && (shown + 1) % 300 == 0) {
            char name[96];
            std::snprintf(name, sizeof name, "%s/shot_%05u.png", BASE.c_str(), shown + 1);
            write_png(name, pix[ri], video::W, video::H);
        }
        ri ^= 1;
        sceKernelSignalSema(buf_free, 1);   // the texture holds a copy: the buffer can be drawn again
        uint64_t p1 = sceKernelGetProcessTimeWide();
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        // full screen by default; the file ux0:data/nbbatman/aspect43 switches to the arcade's 4:3, centred
        float w = aspect43 ? 725.f : 960.f, x0 = (960.f - w) / 2, x1 = x0 + w;
        float u1 = video::W / 512.f, v1 = video::H / 256.f;
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0);   glVertex2f(x0, 0);
        glTexCoord2f(u1, 0);  glVertex2f(x1, 0);
        glTexCoord2f(0, v1);  glVertex2f(x0, 544);
        glTexCoord2f(u1, v1); glVertex2f(x1, 544);
        glEnd();
        vglSwapBuffers(GL_FALSE);
        uint64_t p2 = sceKernelGetProcessTimeWide();
        upload_us_acc += p1 - p0; swap_us_acc += p2 - p1;
        if (++shown % 60 == 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t ef = emu_frames.exchange(0), eu = emu_us_acc.exchange(0);
            double secs = (now - t_last) / 1e6;
            std::printf("[prof] fps %.1f | emu %.2f ms/frame (%llu frames) | render %.2f ms | sound %.0f%% of a core | interpreted %llu\n",
                        60 / secs, ef ? eu / 1000.0 / ef : 0.0, (unsigned long long)ef, render_us_acc.exchange(0) / 1000.0 / 60,
                        audio_us_acc.exchange(0) / 1e4 / secs, (unsigned long long)rec::stats.interpreted);
            std::printf("[prof2] top %.2f | bottom %.2f | band wait %.2f | upload %.2f | swap %.2f ms\n",
                        top_us_acc.exchange(0) / 60e3, bottom_us_acc.exchange(0) / 60e3, band_wait_us_acc.exchange(0) / 60e3,
                        upload_us_acc.exchange(0) / 60e3, swap_us_acc.exchange(0) / 60e3);
            t_last = now;
        }
    }
}

int main() {
#ifndef NBB_DUMP
    sceIoMkdir(BASE.c_str(), 0777);
#endif
    { SceIoStat st; aspect43 = sceIoGetstat((BASE + "/aspect43").c_str(), &st) >= 0; }
    std::printf("Ninja Baseball Bat Man (native V33 recompilation) - Vita build\n");
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    band_go = sceKernelCreateSema("band_go", 0, 0, 1, nullptr);
    band_done = sceKernelCreateSema("band_done", 0, 0, 1, nullptr);
    buf_free = sceKernelCreateSema("buf_free", 0, 2, 2, nullptr);
    buf_ready = sceKernelCreateSema("buf_ready", 0, 0, 2, nullptr);
    SceUID bt = sceKernelCreateThread("band", band_thread, 0x50, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (bt < 0) fatal("band thread create failed");
    sceKernelStartThread(bt, 0, nullptr);
    SceUID pt = sceKernelCreateThread("present", present_thread, 0x48, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);   // core 0: core 2 is shared with the audio thread (higher priority)
    if (pt < 0) fatal("present thread create failed");
    sceKernelStartThread(pt, 0, nullptr);
    SceUID rt = sceKernelCreateThread("render", render_thread, 0x48, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (rt < 0) fatal("render thread create failed");
    sceKernelStartThread(rt, 0, nullptr);
    SceUID at = sceKernelCreateThread("audio", audio_thread, 0x40, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (at >= 0) sceKernelStartThread(at, 0, nullptr);
    state_sem = sceKernelCreateSema("state", 0, 0, 1, nullptr);
    SceUID wt = sceKernelCreateThread("statewriter", state_writer_thread, 0xA0, 128 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (wt >= 0) sceKernelStartThread(wt, 0, nullptr);
    SceUID et = sceKernelCreateThread("emu", emu_thread, 0x60, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (et < 0) fatal("emu thread create failed");
    sceKernelStartThread(et, 0, nullptr);
    sceKernelWaitThreadEnd(et, nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
