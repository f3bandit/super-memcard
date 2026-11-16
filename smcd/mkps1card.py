#!/usr/bin/env python3
"""A PS1 memory card image (128 KB) for tests: mkps1card.py <out> [<save name>:<fill byte>]...
Each save one block, named as given (BASLUS-00002TEST), its data the fill byte after the "SC" header."""
import sys
F, B = 128, 8192
img = bytearray(B * 16)
def chk(fr):
    x = 0
    for b in fr[:127]:
        x ^= b
    fr[127] = x
def frame(i):
    return memoryview(img)[i * F:(i + 1) * F]
h = bytearray(F); h[0:2] = b'MC'; chk(h); img[0:F] = h; img[63 * F:64 * F] = h
for b in range(1, 16):
    f = bytearray(F); f[0] = 0xA0; f[8] = f[9] = 0xFF; chk(f); img[b * F:(b + 1) * F] = f
for k in range(16, 36):
    f = bytearray(F); f[0:4] = b'\xff\xff\xff\xff'; f[8] = f[9] = 0xFF; chk(f); img[k * F:(k + 1) * F] = f
for n, spec in enumerate(sys.argv[2:]):
    name, fill = spec.rsplit(':', 1)
    b = n + 1
    f = bytearray(F); f[0] = 0x51; f[4:8] = (B).to_bytes(4, 'little'); f[8] = f[9] = 0xFF
    f[0x0A:0x0A + len(name)] = name.encode(); chk(f); img[b * F:(b + 1) * F] = f
    blk = bytearray([int(fill, 0)]) * B; blk[0:2] = b'SC'; blk[2] = 0x11; blk[3] = 1
    img[b * B:(b + 1) * B] = blk
open(sys.argv[1], 'wb').write(img)
