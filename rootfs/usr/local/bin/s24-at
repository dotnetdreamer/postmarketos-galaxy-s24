#!/usr/bin/env python3
"""Send AT commands to a Samsung cpif channel and print the replies (Galaxy S24, Claude 2026-11-14).
  at.py DEVICE "AT" "AT+CPIN?" ...     each command is sent with CR, replies read for up to 3 s"""
import os
import select
import sys
import time

dev = sys.argv[1]
fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
for cmd in sys.argv[2:]:
    os.write(fd, (cmd + "\r").encode())
    out, end = b"", time.time() + 3
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try:
                d = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if d:
                out += d
                if b"OK" in out or b"ERROR" in out:
                    break
    print("%-14s -> %r" % (cmd, out))
os.close(fd)
