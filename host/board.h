// The Irem M92 board (Ninja Baseball Bat Man), main CPU side: memory map, I/O ports, the uPD71059C interrupt
// controller, sprite/palette DMA, inputs. Behaviour from MAME's m92 driver as documented in the README; our own code.
#pragma once
#include <cstdint>
#include <vector>

struct V33;

struct State;
namespace board {
extern std::vector<uint8_t> rom;          // 1 MB V33 program (h0/l0 at 0, h1/l1 at 0x80000), byte-addressed
extern uint8_t ram[0x10000];              // 0xe0000
extern uint8_t vram[0x10000];             // 0xd0000: three playfields (bases from the master control registers)
extern uint16_t spriteram[0x800];         // 0xf8000: two CPU-visible banks of 0x400 words (videocontrol bit 0)
extern uint16_t spriteram_buffer[0x400];  // what the sprite chip draws (filled by DMA)
extern uint16_t paletteram[0x1000];       // 0xf8800: four CPU banks of 0x400 words (videocontrol bits 1-2)
extern uint16_t palette[0x2000];          // the palette the screen uses (xBGR555), filled by DMA
extern uint16_t dmacontrol[8], videocontrol;
extern uint16_t pf_control[3][4], master_control[4];
extern int bank;                          // ROM bank at 0xa0000 (port 0x20 bits 1-2)
extern int raster_line;                   // IR2 line
// Video registers as they were when the raster interrupt fired: the screen above that line is drawn with them
// (MAME draws the screen up to the raster line before the interrupt). Collected per frame, latched at vblank.
struct VideoRegs { uint16_t pf_control[3][4], master_control[4], videocontrol; };
constexpr int MAX_SPLITS = 8;
struct Splits { int n; int line[MAX_SPLITS]; VideoRegs regs[MAX_SPLITS]; uint8_t vram[MAX_SPLITS][0x10000]; };
void swap_splits(Splits*& other);         // hands over the last completed frame's splits (video::snapshot)
void raster_split(int line);              // called at the raster line, before IR2 is raised
void latch_splits();                      // called at vblank
extern bool dma_busy;
extern uint16_t in_p1p2, in_coins, in_dsw; // active low
extern uint8_t sound_latch, sound_reply;
extern bool sound_reset;
void init();
void state(::State& st);         // ROM bank, RAM, video and DMA state (machine.cpp remaps the CPU pages afterwards)
void map_pages(V33& cpu);          // direct ROM/RAM pages for the CPU (again after a bank switch)
void dma_finished();                      // the scheduler calls this when the DMA transfer time has passed

uint8_t rd8(uint32_t a);
void wr8(uint32_t a, uint8_t v);
uint8_t in8(uint16_t port);
void out8(uint16_t port, uint8_t v);
}

void sched_dma_after(int pixel_clocks);   // scheduler: call board::dma_finished() after this time

// uPD71059C (8259-compatible), single, as the M92 uses it
namespace pic {
void state(::State& st);
void reset();
void set_ir(int line, bool level);        // edge-triggered (or level if ICW1 says so)
bool int_pending();                       // the INT output to the V33
int ack();                                // INTA: returns the vector number
uint8_t read(int a0);
void write(int a0, uint8_t v);
}
