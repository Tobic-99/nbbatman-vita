#!/usr/bin/env python3
"""make_main_bin.py: interleave the V33 program ROMs into rom/main.bin (1 MB), the input of the translator."""
import os
R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'rom')
out = bytearray(0x100000)
for base, lo, hi in ((0, 'a1-l0-a.31', 'a1-h0-a.34'), (0x80000, 'a1-l1-.32', 'a1-h1-.33')):
    a, b = (open(os.path.join(R, n), 'rb').read() for n in (lo, hi))
    out[base:base + 2 * len(a):2] = a
    out[base + 1:base + 2 * len(b):2] = b
open(os.path.join(R, 'main.bin'), 'wb').write(out)
