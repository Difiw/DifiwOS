#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png  - minimal P6 -> PNG converter (stdlib only).
SPDX-License-Identifier: MIT
"""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
# header: P6\nW H\n255\n
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
pix = parts[3]
raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
    chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
