#!/usr/bin/env python3
"""Converts Difiw .dicon (u32 w, u32 h, w*h*4 bytes) to PNG. Usage: dicon2png.py in.dicon out.png [order]"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack_from('<II', d)
px = d[8:8 + w * h * 4]
order = sys.argv[3] if len(sys.argv) > 3 else 'rgba'
if order != 'rgba':
    b = bytearray(px)
    idx = [order.index(c) for c in 'rgba']
    for i in range(0, len(b), 4):
        o = px[i:i + 4]
        b[i:i + 4] = bytes(o[j] for j in idx)
    px = bytes(b)
raw = b''.join(b'\0' + px[y * w * 4:(y + 1) * w * 4] for y in range(h))
def ch(t, c):
    x = t + c
    return struct.pack('>I', len(c)) + x + struct.pack('>I', zlib.crc32(x))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))
