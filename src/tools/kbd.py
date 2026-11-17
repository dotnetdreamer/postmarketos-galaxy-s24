#!/usr/bin/env python3
"""Virtual keyboard through /dev/uinput (Claude test helper for the pmOS S24, 2026-11-15).
usage: kbd.py STEP...   steps: k:NAME (press+release)  t:TEXT (type digits/letters/space)
                               s:SECONDS (sleep)  h:NAME:SECONDS (hold)
The device lives only while the script runs; it waits 2.5 s after creating it so
udev + libinput in the compositor can pick it up."""
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1
KEYS = {"ESC": 1, "BACKSPACE": 14, "TAB": 15, "ENTER": 28, "LEFTCTRL": 29, "LEFTSHIFT": 42,
        "SPACE": 57, "UP": 103, "LEFT": 105, "RIGHT": 106, "DOWN": 108, "POWER": 116,
        "LEFTMETA": 125, "LEFTALT": 56, "VOLUMEDOWN": 114, "VOLUMEUP": 115}
for i, ch in enumerate("1234567890"):
    KEYS[ch] = 2 + i
for row, base in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, ch in enumerate(row):
        KEYS[ch] = base + i


def main(steps):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for code in range(1, 249):
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 0x06, 0x1d6b, 0x0104, 1, b"claude-test-keyboard", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    print("created", flush=True)
    time.sleep(2.5)

    def ev(t, c, v):
        now = time.time()
        os.write(fd, struct.pack("qqHHi", int(now), int((now % 1) * 1e6), t, c, v))

    def key(code, down):
        ev(EV_KEY, code, 1 if down else 0)
        ev(EV_SYN, 0, 0)

    try:
        for step in steps:
            kind, _, arg = step.partition(":")
            if kind == "s":
                time.sleep(float(arg))
            elif kind == "k":
                key(KEYS[arg], True); time.sleep(0.06); key(KEYS[arg], False); time.sleep(0.12)
            elif kind == "h":
                name, _, secs = arg.partition(":")
                key(KEYS[name], True); time.sleep(float(secs)); key(KEYS[name], False); time.sleep(0.12)
            elif kind == "t":
                for ch in arg:
                    code = KEYS["SPACE" if ch == " " else ch]
                    key(code, True); time.sleep(0.05); key(code, False); time.sleep(0.15)
            else:
                raise SystemExit("bad step " + step)
            print("done", step if kind != "t" else "t:<%d chars>" % len(arg), flush=True)
        time.sleep(0.5)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
        print("destroyed", flush=True)


if __name__ == "__main__":
    main(sys.argv[1:])
