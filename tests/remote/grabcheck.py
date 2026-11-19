#!/usr/bin/env python3
# grabcheck.py: is the touchscreen held by someone? (EVIOCGRAB fails with EBUSY if it is; if ours
# succeeds, nobody held it, and it is let go at once.)
import fcntl, os, errno
EVIOCGRAB = 0x40044590
for n in sorted(os.listdir("/sys/class/input")):
    if not n.startswith("event"): continue
    name = open("/sys/class/input/%s/device/name" % n).read().strip()
    if "touchscreen" not in name.lower(): continue
    fd = os.open("/dev/input/" + n, os.O_RDONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        fcntl.ioctl(fd, EVIOCGRAB, 0)
        print(n, name, ": not held by anyone")
    except OSError as e:
        print(n, name, ": held by someone else" if e.errno == errno.EBUSY else ": %s" % e)
    os.close(fd)
