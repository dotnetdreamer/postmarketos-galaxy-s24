#!/usr/bin/python3
# print input events from devices (by name) for N seconds
import os, sys, struct, time, glob, select
names = sys.argv[1].split(',')
secs = float(sys.argv[2])
fds = {}
for d in glob.glob('/sys/class/input/event*'):
    n = open(d + '/device/name').read().strip()
    if n in names:
        fds[os.open('/dev/input/' + os.path.basename(d), os.O_RDONLY | os.O_NONBLOCK)] = n
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select(list(fds), [], [], max(0, end - time.time()))
    for fd in r:
        data = os.read(fd, 24 * 64)
        for i in range(0, len(data), 24):
            s, us, t, c, v = struct.unpack('<qqHHi', data[i:i + 24])
            if t != 0:
                print('%.3f %s type=%d code=%d value=%d' % (s + us / 1e6, fds[fd], t, c, v), flush=True)
