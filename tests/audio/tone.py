#!/usr/bin/env python3
# tone.py SECONDS DBFS FREQ OUT : raw S16_LE stereo 48 kHz sine (Claude, 2026-11-15)
import math, struct, sys
sec, db, f, out = float(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
a = 32767 * 10 ** (db / 20)
n = int(48000 * sec)
fade = 4800
with open(out, "wb") as o:
    buf = bytearray()
    for i in range(n):
        g = min(1.0, i / fade, (n - i) / fade)
        v = int(a * g * math.sin(2 * math.pi * f * i / 48000))
        buf += struct.pack("<hh", v, v)
    o.write(buf)
