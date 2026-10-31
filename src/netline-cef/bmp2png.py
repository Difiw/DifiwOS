#!/usr/bin/env python3
"""Converts the 32-bit BMP written by SDL_SaveBMP (NETLINE_SHOT test hook) to PNG. Usage: bmp2png.py in.bmp out.png"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', d, 10)[0]
w, h = struct.unpack_from('<ii', d, 18)
bpp = struct.unpack_from('<H', d, 28)[0]
bottom_up = h > 0; h = abs(h); n = bpp // 8
stride = (w * n + 3) & ~3
rows = []
for y in range(h):
    yy = h - 1 - y if bottom_up else y
    r = d[off + yy * stride: off + yy * stride + w * n]
    out = bytearray()
    for x in range(w):
        b, g, rr = r[x*n], r[x*n+1], r[x*n+2]
        out += bytes((rr, g, b))
    rows.append(b'\0' + bytes(out))
def ch(t, c):
    x = t + c
    return struct.pack('>I', len(c)) + x + struct.pack('>I', zlib.crc32(x))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(b''.join(rows), 6)) + ch(b'IEND', b''))
