#!/usr/bin/python3
# inject a key press+release into an existing input device (by name), like the hardware would
import os, sys, struct, time, glob
name, code = sys.argv[1], int(sys.argv[2])
hold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
for d in glob.glob('/sys/class/input/event*'):
    if open(d + '/device/name').read().strip() == name:
        dev = '/dev/input/' + os.path.basename(d)
        break
else:
    sys.exit('no device ' + name)
fd = os.open(dev, os.O_WRONLY)
def ev(t, c, v):
    now = time.time()
    os.write(fd, struct.pack('<qqHHi', int(now), int((now % 1) * 1e6), t, c, v))
ev(1, code, 1); ev(0, 0, 0)
time.sleep(hold)
ev(1, code, 0); ev(0, 0, 0)
print('injected', code, 'into', dev, name)
