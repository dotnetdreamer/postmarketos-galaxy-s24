#!/usr/bin/env python3
# rawwin.py FILE CHANNELS WIN_MS : RMS dBFS of channel 0 per window (Claude, 2026-11-15)
import math, struct, sys
d = open(sys.argv[1], "rb").read()
ch, win = int(sys.argv[2]), int(48 * int(sys.argv[3]))
v = struct.unpack("<%dh" % (len(d) // 2), d[: len(d) // 2 * 2])[0::ch]
out = []
for i in range(0, len(v) - win + 1, win):
    s = v[i:i + win]
    r = math.sqrt(sum(x * x for x in s) / len(s))
    out.append("%.0f" % (20 * math.log10(r / 32768) if r else -99))
print(" ".join(out))
