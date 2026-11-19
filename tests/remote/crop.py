#!/usr/bin/env python3
# crop.py IN.ppm OUT.ppm X Y W H: a full-resolution piece of a P6 picture.
import sys
src, dst = sys.argv[1], sys.argv[2]
X, Y, W, H = (int(v) for v in sys.argv[3:7])
f = open(src, "rb")
head = f.readline().split()
w, h = int(head[1]), int(head[2])
f.seek(len(b"P6 %d %d 255\n" % (w, h)) + Y * w * 3)
out = bytearray()
for y in range(H):
    line = f.read(w * 3)
    out += line[X * 3:(X + W) * 3]
open(dst, "wb").write(b"P6 %d %d 255\n" % (W, H) + bytes(out))
