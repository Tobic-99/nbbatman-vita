#!/usr/bin/env python3
"""Split a Vita frame dump into one state file per raster region for tools/mame_inject.lua (MAME as a renderer reference)."""
import struct, sys
src, out = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
VRAM = 0x10000; ox = VRAM + 0x4000 + 0x800; RS = 34
o = (ox + RS + 3) & ~3
n = struct.unpack_from('<i', d, o)[0]; lines = struct.unpack_from('<8i', d, o + 4)
vr = o + 36 + 8 * RS
top = 0; regions = []
for i in range(n + 1):
    last = i == n
    bottom = 240 if last else lines[i] - 8 + 1
    regs = d[ox:ox + RS] if last else d[o + 36 + i * RS:o + 36 + (i + 1) * RS]
    vram = d[:VRAM] if last else d[vr + i * VRAM:vr + (i + 1) * VRAM]
    open(f'{out}.r{i}', 'wb').write(vram + regs + d[VRAM:VRAM + 0x4000 + 0x800])
    regions.append((top, max(top, min(bottom, 240)))); top = max(top, bottom)
open(f'{out}.regions', 'w').write(' '.join(f'{a}:{b}' for a, b in regions))
