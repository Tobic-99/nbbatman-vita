# How the port runs on the PS Vita

The M92 board runs at 60.05 Hz (262 lines of 424 pixels at 6.67 MHz). Measured on a PS Vita at 444 MHz, resumed in a stage with no buttons pressed: 59 fps,
the translated V33 code about 3 ms per frame, drawing 7 ms (split over two cores), the sound about 60% of a core. Busier scenes were not measured with this build.

## Threads

| Thread | Core | Work |
|---|---|---|
| emu | 0 | translated V33 code, board, one frame at a time, paced to 60.05 Hz |
| band | 0 | draws the bottom third of the frame, above emu in priority |
| present | 0 | copies the finished frame to a texture, shows it |
| render | 1 | draws the top two thirds of the frame |
| statewriter | 1 | writes save states and high scores in the background |
| audio | 2 | V35, YM2151 and GA20, paced by the audio output |

## CPU

- `tools/v33rec.py` writes one C++ routine per guest routine. Routines end at calls, returns and I/O and hand CS:IP to a dispatcher
  (`host/rec.cpp`) which takes interrupts. Anything untranslated runs in the interpreter (`host/v33.cpp`).
- Entry points come from static discovery and from targets recorded at run time (`tools/v33_runtime_targets.txt`, addresses only).
- Instruction costs are a flat estimate, corrected by one factor (`CYCLE_CALIBRATION`, `host/machine.cpp`) measured against the
  frames on which MAME sees the game's sound commands.

## Sound

The V35 driver spends about 97% of its instructions in an idle loop. `host/sound.cpp` replays it arithmetically with the same cycle
costs, so the output stays byte-identical at a fraction of the work.

## Video

- A software renderer (`host/video.cpp`). Each frame's state is copied at vblank, so drawing does not hold up the CPU; raster
  splits hand over their copies by pointer.
- Tile rows and sprite rows with nothing to draw are skipped through tables built at start.
- The finished frame is copied straight into the texture's memory: `glTexSubImage2D` took 8.6 ms a frame, the copy about 1 ms. Two
  textures keep the GPU from reading the one being written.
