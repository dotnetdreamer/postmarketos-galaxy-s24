#!/usr/bin/env python3
# powerkey.py (root): one press of the power key, written into the power key's own input device, the
# way s24-powerd injects it (everything reading the device sees it: KWin, s24-remote).
import os, struct, time
for n in sorted(os.listdir("/sys/class/input")):
    if n.startswith("event") and "power-keys" in open("/sys/class/input/%s/device/name" % n).read():
        fd = os.open("/dev/input/" + n, os.O_WRONLY)
        def ev(t, c, v):
            s, us = divmod(int(time.time() * 1e6), 1000000)
            os.write(fd, struct.pack("llHHi", s, us, t, c, v))
        ev(1, 116, 1); ev(0, 0, 0)
        time.sleep(0.1)
        ev(1, 116, 0); ev(0, 0, 0)
        os.close(fd)
        print("pressed power on", n)
        break
