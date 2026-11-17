#!/usr/bin/env python3
# rawstat.py FILE [CHANNELS] : peak / RMS per channel of raw S16_LE audio (Claude, 2026-11-15)
import math, struct, sys
d = open(sys.argv[1], "rb").read()
ch = int(sys.argv[2]) if len(sys.argv) > 2 else 2
n = len(d) // 2
v = struct.unpack("<%dh" % n, d[: n * 2])
for c in range(ch):
    s = v[c::ch]
    if not s:
        continue
    pk = max(abs(x) for x in s)
    rms = math.sqrt(sum(x * x for x in s) / len(s))
    db = 20 * math.log10(rms / 32768) if rms > 0 else -999
    print("ch%d: %d frames, peak %d, rms %.1f (%.1f dBFS), nonzero %d" % (c, len(s), pk, rms, db, sum(1 for x in s if x)))
