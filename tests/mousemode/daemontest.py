#!/usr/bin/python3
"""Tests for s24-mouse-moded with a stand-in touchscreen (Claude, 2026-11-15). Run as root on the
phone (in the pmOS root). The daemon runs on its own socket with --device <stand-in>, --dpms <a
file this test writes>, so the real touchscreen is never touched. The stand-in and the daemon's
touchpad are disabled in KWin (kcminputrc), so nothing reaches the session: the touchpad's
output is read here, straight from its event node."""
import fcntl, glob, os, select, socket, struct, subprocess, sys, time

DAEMON = '/usr/local/sbin/s24-mouse-moded'
SOCK = '/tmp/mmtest.sock'
DPMS = '/tmp/mmtest-dpms'
PAD_NAME = 'S24 mouse mode touchpad'

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
EVIOCGRAB = 0x40044590
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP = 0x14a, 0x145, 0x14d, 0x14e
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TOUCH_MAJOR = 0, 1, 0x2f, 0x30
ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x35, 0x36, 0x39
INPUT_PROP_DIRECT = 1
EV = struct.Struct('llHHi')
PW, PH = 2636, 5716   # the daemon's touchpad units for the panel (1/40 mm)

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


class StandIn:
    """A touchscreen like sec_touchscreen: 0..4095 both ways, 10 slots, direct."""
    def __init__(self):
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for k in (BTN_TOUCH, BTN_TOOL_FINGER):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for code, mx in ((ABS_X, 4095), (ABS_Y, 4095), (ABS_MT_SLOT, 9), (ABS_MT_TOUCH_MAJOR, 255),
                         (ABS_MT_POSITION_X, 4095), (ABS_MT_POSITION_Y, 4095), (ABS_MT_TRACKING_ID, 65535)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack('H2x6i', code, 0, 0, mx, 0, 0, 0))
        fcntl.ioctl(self.fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 6, 0x1234, 0x5678, 1, b'S24 test touchscreen', 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.ids = {}
        self.next = 100
        self.path = None
        for _ in range(50):
            for p in glob.glob('/sys/class/input/event*/device/name'):
                if open(p).read().strip() == 'S24 test touchscreen':
                    self.path = '/dev/input/' + p.split('/')[4]
            if self.path and os.path.exists(self.path):
                break
            time.sleep(0.05)

    def _w(self, t, c, v):
        os.write(self.fd, EV.pack(0, 0, t, c, v))

    def frame(self, touches):
        """touches: {slot: (x, y) or None}; None lifts it."""
        for s, pos in touches.items():
            self._w(EV_ABS, ABS_MT_SLOT, s)
            if pos is None:
                if s in self.ids:
                    self._w(EV_ABS, ABS_MT_TRACKING_ID, -1)
                    del self.ids[s]
                continue
            if s not in self.ids:
                self.ids[s] = self.next
                self.next += 1
                self._w(EV_ABS, ABS_MT_TRACKING_ID, self.ids[s])
            self._w(EV_ABS, ABS_MT_POSITION_X, pos[0])
            self._w(EV_ABS, ABS_MT_POSITION_Y, pos[1])
        self._w(EV_KEY, BTN_TOUCH, 1 if self.ids else 0)
        self._w(EV_SYN, SYN_REPORT, 0)

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


class Pad:
    def __init__(self):
        self.fd = None

    def find(self, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            for p in glob.glob('/sys/class/input/event*/device/name'):
                if open(p).read().strip() == PAD_NAME:
                    path = '/dev/input/' + p.split('/')[4]
                    try:
                        if self.fd is not None:
                            os.close(self.fd)
                        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                        return path
                    except OSError:
                        pass
            time.sleep(0.05)
        return None

    def exists(self):
        return any(open(p).read().strip() == PAD_NAME for p in glob.glob('/sys/class/input/event*/device/name'))

    def read(self, wait=0.15):
        """All events now readable, as frames: [[(type, code, value), ...], ...]."""
        time.sleep(wait)
        frames, cur = [], []
        while self.fd is not None:
            try:
                data = os.read(self.fd, EV.size * 64)
            except BlockingIOError:
                break
            except OSError:
                break
            for i in range(0, len(data), EV.size):
                _, _, t, c, v = EV.unpack_from(data, i)
                if t == EV_SYN and c == SYN_REPORT:
                    frames.append(cur)
                    cur = []
                else:
                    cur.append((t, c, v))
        return frames


class Client:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(SOCK)
        self.s.setblocking(False)
        self.buf = b''

    def send(self, line):
        self.s.sendall((line + '\n').encode())

    def lines(self, wait=0.2):
        end = time.time() + wait
        out = []
        while True:
            left = end - time.time()
            if left <= 0:
                break
            r, _, _ = select.select([self.s], [], [], left)
            if not r:
                break
            data = self.s.recv(4096)
            if not data:
                out.append('<eof>')
                break
            self.buf += data
        while b'\n' in self.buf:
            l, self.buf = self.buf.split(b'\n', 1)
            out.append(l.decode())
        return out


def grab_free(path):
    """Can this test grab the stand-in? Then the daemon doesn't hold it."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        fcntl.ioctl(fd, EVIOCGRAB, 0)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


def keys(frames):
    return [(c, v) for f in frames for (t, c, v) in f if t == EV_KEY]


def last_pos(frames):
    x = y = None
    for f in frames:
        for t, c, v in f:
            if t == EV_ABS and c == ABS_MT_POSITION_X:
                x = v
            if t == EV_ABS and c == ABS_MT_POSITION_Y:
                y = v
    return x, y


def main():
    with open(DPMS, 'w') as f:
        f.write('On\n')
    ts = StandIn()
    check('stand-in touchscreen made', ts.path, ts.path)
    log = open('/tmp/mmtest-daemon.log', 'w')
    d = subprocess.Popen([DAEMON, '-v', '--device', ts.path, '--dpms', DPMS, '--socket', SOCK], stdout=log, stderr=log)
    time.sleep(0.4)
    pad = Pad()
    try:
        c = Client()
        c.send('status')
        check('status answers off', c.lines() == ['state off'])

        c.send('on')
        ev = c.lines()
        check('on -> state on', 'state on' in ev, ev)
        path = pad.find()
        check('touchpad device appears', path, path)
        check('daemon holds the touchscreen', not grab_free(ts.path))
        pad.read(0.05)

        # one finger: down, move, up
        ts.frame({0: (2048, 2048)})
        f = pad.read()
        k = keys(f)
        check('1 finger: BTN_TOUCH and BTN_TOOL_FINGER down', (BTN_TOUCH, 1) in k and (BTN_TOOL_FINGER, 1) in k, k)
        check('1 finger: position in 1/40 mm', last_pos(f) == (2048 * PW // 4095, 2048 * PH // 4095), last_pos(f))
        ts.frame({0: (2148, 2048)})
        f = pad.read()
        check('1 finger: moves', last_pos(f)[0] == 2148 * PW // 4095, last_pos(f))
        ts.frame({0: None})
        f = pad.read()
        k = keys(f)
        tid = [v for fr in f for (t, cc, v) in fr if t == EV_ABS and cc == ABS_MT_TRACKING_ID]
        check('1 finger: lifted', (BTN_TOUCH, 0) in k and (BTN_TOOL_FINGER, 0) in k and tid == [-1], (k, tid))

        # two fingers
        ts.frame({0: (1500, 2000), 1: (2500, 2000)})
        f = pad.read()
        check('2 fingers: BTN_TOOL_DOUBLETAP', (BTN_TOOL_DOUBLETAP, 1) in keys(f), keys(f))
        ts.frame({0: None, 1: None})
        f = pad.read()
        check('2 fingers: lifted', (BTN_TOOL_DOUBLETAP, 0) in keys(f) and (BTN_TOUCH, 0) in keys(f), keys(f))

        # swipe in from the bottom edge: an "edge bottom", nothing on the touchpad
        c.lines(0.05)
        ts.frame({0: (2048, 4090)})
        time.sleep(0.03)
        for y in (4000, 3900, 3790):
            ts.frame({0: (2048, y)})
            time.sleep(0.02)
        ev = c.lines(0.2)
        f = pad.read(0.05)
        check('bottom edge swipe -> edge bottom', 'edge bottom' in ev, ev)
        check('bottom edge swipe: nothing on the touchpad', f == [], f)
        ts.frame({0: None})
        f = pad.read()
        check('bottom edge swipe: let go, still nothing', f == [], f)

        # top edge swipe
        ts.frame({0: (2048, 5)})
        for y in (100, 200, 310):
            time.sleep(0.02)
            ts.frame({0: (2048, y)})
        ev = c.lines(0.2)
        check('top edge swipe -> edge top', 'edge top' in ev, ev)
        ts.frame({0: None})
        pad.read(0.05)

        # a quick touch at the top edge: a tap on the touchpad
        ts.frame({0: (2048, 5)})
        time.sleep(0.05)
        ts.frame({0: None})
        f = pad.read()
        k = keys(f)
        check('edge tap -> one tap on the touchpad', k.count((BTN_TOUCH, 1)) == 1 and k.count((BTN_TOUCH, 0)) == 1, f)

        # an edge touch that goes sideways is the pointer
        ts.frame({0: (1000, 4090)})
        time.sleep(0.03)
        ts.frame({0: (1250, 4090)})
        f1 = pad.read(0.05)
        ts.frame({0: (1500, 4090)})
        f2 = pad.read()
        check('edge touch sideways: held back first', f1 == [], f1)
        check('edge touch sideways: then the pointer', (BTN_TOUCH, 1) in keys(f2) and last_pos(f2)[0] == 1500 * PW // 4095, f2)
        ts.frame({0: None})
        pad.read(0.05)

        # an edge touch that waits is the pointer after 0.7 s
        ts.frame({0: (2048, 4090)})
        f1 = pad.read(0.3)
        f2 = pad.read(0.6)
        check('edge touch waiting: the pointer after 0.7 s', f1 == [] and (BTN_TOUCH, 1) in keys(f2), (f1, keys(f2)))
        ts.frame({0: None})
        pad.read(0.05)

        # pause / resume
        c.lines(0.05)
        c.send('pause')
        ev = c.lines()
        check('pause -> state paused', 'state paused' in ev, ev)
        check('paused: touchscreen let go', grab_free(ts.path))
        ts.frame({0: (2048, 2048)})
        f = pad.read()
        check('paused: nothing on the touchpad', f == [], f)
        c.send('resume')
        ev = c.lines()
        check('resume while a finger is down: not taken yet', 'state on' in ev and grab_free(ts.path), ev)
        ts.frame({0: None})
        time.sleep(0.1)
        check('resume: taken once the finger goes', not grab_free(ts.path))
        f = pad.read()
        check('resume: that touch never reached the touchpad', f == [], f)

        # rotation: a quarter turn swaps the touchpad's axes
        c.send('rotate 1')
        time.sleep(0.4)
        path = pad.find()
        ts.frame({0: (2048, 2048)})
        f = pad.read()
        px, py = 2048 * PW // 4095, 2048 * PH // 4095
        check('rotate 1: new touchpad, x = panel y, y = width - panel x', last_pos(f) == (py, PW - px), last_pos(f))
        ts.frame({0: None})
        pad.read(0.05)
        c.send('rotate 0')
        time.sleep(0.4)
        pad.find()

        # three fingers held: mouse mode ends
        c.lines(0.05)
        ts.frame({0: (1000, 2000), 1: (2000, 2000), 2: (3000, 2000)})
        f = pad.read(0.1)
        check('3 fingers: BTN_TOOL_TRIPLETAP', (BTN_TOOL_TRIPLETAP, 1) in keys(f), keys(f))
        ev = c.lines(1.0)
        f = pad.read(0.05)
        check('3 fingers held 0.8 s -> exit hold', 'exit hold' in ev and 'state off' in ev, ev)
        check('3 fingers held: lifted on the touchpad', (BTN_TOUCH, 0) in keys(f), keys(f))
        check('3 fingers held: touchpad stays while fingers are down', pad.exists())
        ts.frame({0: None, 1: None, 2: None})
        time.sleep(0.2)
        check('3 fingers held: then the touchpad goes', not pad.exists())
        check('3 fingers held: touchscreen let go', grab_free(ts.path))

        # the panel goes dark: mouse mode ends at once
        c.send('on')
        c.lines()
        pad.find()
        ts.frame({0: (2048, 2048)})
        pad.read(0.05)
        with open(DPMS, 'w') as f_:
            f_.write('Off\n')
        # read once no touch has come for a second (the session half hears it from KWin at once)
        ev = c.lines(2.5)
        check('panel dark -> exit dark (within 2.5 s of the last touch)', 'exit dark' in ev, ev)
        check('panel dark: let go at once, finger or not', grab_free(ts.path))
        check('panel dark: touchpad gone at once', not pad.exists())
        ts.frame({0: None})
        c.send('on')
        ev = c.lines()
        check('on while dark -> refused', 'exit dark' in ev and 'state off' in ev, ev)
        with open(DPMS, 'w') as f_:
            f_.write('On\n')

        # someone else may not connect
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.setgid(65534)
            os.setuid(65534)
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(SOCK)
                s.sendall(b'status\n')
                s.settimeout(1)
                data = s.recv(100)
                os.write(w, b'answered' if data else b'closed')
            except OSError as e:
                os.write(w, ('error %s' % e).encode())
            os._exit(0)
        os.waitpid(pid, 0)
        got = os.read(r, 100).decode()
        check('a stranger is refused', got in ('closed', 'error [Errno 104] Connection reset by peer') or got.startswith('error'), got)

        # the client that turned it on goes away: off, touchscreen let go
        c.send('on')
        c.lines()
        c.s.close()
        time.sleep(0.4)
        check('owner gone: touchscreen let go', grab_free(ts.path))
        check('owner gone: touchpad gone', not pad.exists())
    finally:
        d.terminate()
        d.wait(timeout=5)
        ts.close()
        for p in (SOCK, DPMS):
            try:
                os.unlink(p)
            except OSError:
                pass
    print('%d/%d passed' % (sum(ok for _, ok in results), len(results)))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == '__main__':
    sys.exit(main())
