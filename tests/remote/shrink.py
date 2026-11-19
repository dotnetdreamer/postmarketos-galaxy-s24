#!/usr/bin/env python3
# shrink.py IN.ppm OUT.ppm ROWS STEP: the top ROWS rows of a P6 picture, every STEP-th pixel.
import sys
src, dst, rows, step = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = open(src, "rb")
head = f.readline().split()
if len(head) < 4:
    while len(head) < 4: head += f.readline().split()
w, h = int(head[1]), int(head[2])
rows = min(rows, h)
out = bytearray()
for y in range(rows):
    line = f.read(w * 3)
    if y % step: continue
    for x in range(0, w, step):
        out += line[x * 3:x * 3 + 3]
open(dst, "wb").write(b"P6 %d %d 255\n" % ((w + step - 1) // step, (rows + step - 1) // step) + bytes(out))
