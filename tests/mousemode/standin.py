#!/usr/bin/python3
"""A stand-in touchscreen for testing the S24 mouse mode (Claude, 2026-11-15), like sec_touchscreen
(0..4095 both ways, 10 slots). Run as root:  standin.py  (prints the event node, then waits)
Gestures are written to /tmp/mm-standin.fifo, one per line:
  drag X0 Y0 X1 Y1 MS         one finger from (X0,Y0) to (X1,Y1) over MS milliseconds
  tap X Y                     a quick one-finger tap
  tap2 X Y                    a quick two-finger tap (right click)
  scroll X Y DY MS            two fingers moving DY (touch units) together over MS
  pinch3 SCALE MS             three fingers spreading (SCALE > 1) or closing (< 1) around the centre
  hold3 MS                    three fingers held still
  quit
Touch units: 0..4095 across the panel's width (x) and height (y), panel portrait."""
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EV = struct.Struct('llHHi')
FIFO = '/tmp/mm-standin.fifo'


class Touch:
    def __init__(self):
        self.fd = os.open('/dev/uinput', os.O_WRONLY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, 1)
        for k in (0x14a, 0x145):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, 3)
        for code, mx in ((0, 4095), (1, 4095), (0x2f, 9), (0x30, 255), (0x35, 4095), (0x36, 4095), (0x39, 65535)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack('H2x6i', code, 0, 0, mx, 0, 0, 0))
        fcntl.ioctl(self.fd, UI_SET_PROPBIT, 1)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 6, 0x1234, 0x5678, 1, b'S24 test touchscreen', 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.ids, self.next = {}, 1000

    def w(self, t, c, v):
        os.write(self.fd, EV.pack(0, 0, t, c, v))

    def frame(self, touches):
        for s, pos in touches.items():
            self.w(3, 0x2f, s)
            if pos is None:
                if s in self.ids:
                    self.w(3, 0x39, -1)
                    del self.ids[s]
                continue
            if s not in self.ids:
                self.ids[s] = self.next
                self.next += 1
                self.w(3, 0x39, self.ids[s])
            self.w(3, 0x35, int(pos[0]))
            self.w(3, 0x36, int(pos[1]))
        self.w(1, 0x14a, 1 if self.ids else 0)
        self.w(0, 0, 0)

    def path(self):
        import glob
        for p in glob.glob('/sys/class/input/event*/device/name'):
            if open(p).read().strip() == 'S24 test touchscreen':
                return '/dev/input/' + p.split('/')[4]


def moving(t, pts0, pts1, ms):
    steps = max(2, ms // 8)                 # a report every 8 ms, like a 120 Hz touch chip
    for i in range(steps + 1):
        f = i / steps
        t.frame({s: (pts0[s][0] + (pts1[s][0] - pts0[s][0]) * f, pts0[s][1] + (pts1[s][1] - pts0[s][1]) * f)
                 for s in pts0})
        time.sleep(ms / 1000 / steps)
    t.frame({s: None for s in pts0})


def main():
    t = Touch()
    time.sleep(0.5)
    print(t.path(), flush=True)
    if not os.path.exists(FIFO):
        os.mkfifo(FIFO, 0o600)
    while True:
        with open(FIFO) as f:
            for line in f:
                w = line.split()
                if not w:
                    continue
                if w[0] == 'quit':
                    fcntl.ioctl(t.fd, UI_DEV_DESTROY)
                    os.unlink(FIFO)
                    return
                a = [float(x) for x in w[1:]]
                if w[0] == 'drag':
                    moving(t, {0: (a[0], a[1])}, {0: (a[2], a[3])}, int(a[4]))
                elif w[0] == 'tap':
                    t.frame({0: (a[0], a[1])})
                    time.sleep(0.04)
                    t.frame({0: None})
                elif w[0] == 'tap2':
                    t.frame({0: (a[0] - 300, a[1]), 1: (a[0] + 300, a[1])})
                    time.sleep(0.05)
                    t.frame({0: None, 1: None})
                elif w[0] == 'scroll':
                    moving(t, {0: (a[0] - 300, a[1]), 1: (a[0] + 300, a[1])},
                           {0: (a[0] - 300, a[1] + a[2]), 1: (a[0] + 300, a[1] + a[2])}, int(a[3]))
                elif w[0] == 'pinch3':
                    c, r0 = (2048, 2048), 500
                    pts = {0: (-1, 0), 1: (0.5, 0.866), 2: (0.5, -0.866)}
                    p0 = {s: (c[0] + d[0] * r0, c[1] + d[1] * r0) for s, d in pts.items()}
                    p1 = {s: (c[0] + d[0] * r0 * a[0], c[1] + d[1] * r0 * a[0]) for s, d in pts.items()}
                    moving(t, p0, p1, int(a[1]))
                elif w[0] == 'hold3':
                    t.frame({0: (1200, 2000), 1: (2048, 2000), 2: (2900, 2000)})
                    time.sleep(a[0] / 1000)
                    t.frame({0: None, 1: None, 2: None})
                print('done', line.strip(), flush=True)


if __name__ == '__main__':
    main()
