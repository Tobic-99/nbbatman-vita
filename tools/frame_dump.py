#!/usr/bin/env python3
"""Decode a video::Frame dumped by the Vita profile build (host0:/frame_NNN.bin): registers, raster splits, sprite list."""
import struct, sys
VRAM, PAL, SPR = 0x10000, 0x2000, 0x400
REGS = '<12H4HH'
d = open(sys.argv[1], 'rb').read()
ox = VRAM + PAL * 2 + SPR * 2
pal = struct.unpack_from(f'<{PAL}H', d, VRAM)
spr = struct.unpack_from(f'<{SPR}H', d, VRAM + PAL * 2)
def regs(o):
    r = struct.unpack_from(REGS, d, o)
    return f"scrolly/x {[(r[i*4], r[i*4+2]) for i in range(3)]} master {[hex(x) for x in r[12:16]]} video {r[16]:#06x}"
print('vblank', regs(ox))
o = ox + struct.calcsize(REGS)
o = (o + 3) & ~3
n = struct.unpack_from('<i', d, o)[0]; lines = struct.unpack_from('<8i', d, o + 4)
print('splits', n, lines[:n])
for i in range(n): print(f'  [{i}] line {lines[i]}', regs(o + 36 + i * struct.calcsize(REGS)))
vc = struct.unpack_from(REGS, d, ox)[16]
pri_on = vc & 0x2000; print('OBJ priority', bool(pri_on), 'sprites off', bool(vc & 0x80))
print(' i  offs     x    y  code  col pri fl cols rows layer  pal[1..4]')
offs = i = 0
while offs < SPR:
    w0, code, a, xw = spr[offs:offs + 4]
    cols, rows = 1 << ((w0 >> 11) & 3), 1 << ((w0 >> 9) & 3)
    if w0 or code or a or xw:
        col = (a & (0x7f if pri_on else 0xff)) | (0x100 if vc & 0x8000 else 0)
        x = ((xw & 0x1ff) - 16 & 0x1ff) - 80; y = 368 - (w0 & 0x1ff) - 8
        print(f'{i:3} {offs:4x} {x:5} {y:4} {code:5x} {col:4x} {(a >> 7) & 1:3} {(a >> 8) & 3:2} {cols:4} {rows:4} {w0 >> 13:5}  ' + ' '.join(f'{pal[(col * 16 + k) & 0x1fff]:04x}' for k in (1, 2, 3, 4)))
    offs += 4 * cols; i += 1
