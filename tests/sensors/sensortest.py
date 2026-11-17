#!/usr/bin/python3
# read a few events from Samsung shub sensors: enable via /dev/batch_io + ssp_sensor/enable, read /dev/iio:deviceN
import os, sys, fcntl, struct, time, select, glob
NAMES = {1: 'accelerometer_sensor', 5: 'light_sensor', 8: 'proximity_sensor', 27: 'device_orientation',
         29: 'proximity_raw', 37: 'auto_brightness', 55: 'light_seamless_sensor', 52: 'device_orientation_wu'}
SIZES = {1: 6, 5: 4, 8: 1, 27: 1, 29: 12, 37: 5, 55: 4, 52: 1}
def iio_dev(name):
    for d in glob.glob('/sys/bus/iio/devices/iio:device*'):
        if open(d + '/name').read().strip() == name:
            return d
def w(path, val):
    fd = os.open(path, os.O_WRONLY)
    try:
        return os.write(fd, val.encode())
    except OSError as e:
        print('write', path, val, e, flush=True)
    finally:
        os.close(fd)
types = [int(a) for a in sys.argv[1].split(',')]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5
period_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 200
bio = os.open('/dev/batch_io', os.O_RDWR)
fds = {}
for t in types:
    d = iio_dev(NAMES[t])
    w(d + '/buffer/enable', '0')
    w(d + '/buffer/length', '128')
    w(d + '/buffer/enable', '1')
    fds[os.open('/dev/' + os.path.basename(d), os.O_RDONLY | os.O_NONBLOCK)] = t
    fcntl.ioctl(bio, 0xFC00 | t, struct.pack('<qqi4x', 0, period_ms * 1000000, 0))
    w('/sys/class/sensors/ssp_sensor/enable', str(t * 10 + 1))
    print('enabled', t, NAMES[t], d, flush=True)
end = time.time() + secs
n = {t: 0 for t in types}
try:
    while time.time() < end:
        r, _, _ = select.select(list(fds), [], [], max(0, end - time.time()))
        for fd in r:
            t = fds[fd]
            sz = SIZES[t] + 8
            data = os.read(fd, sz * 64)
            for i in range(0, len(data) - sz + 1, sz):
                rec = data[i:i + sz]
                ts = struct.unpack('<Q', rec[SIZES[t]:])[0]
                p = rec[:SIZES[t]]
                if t == 1:
                    v = struct.unpack('<hhh', p)
                elif t in (5, 55):
                    v = struct.unpack('<I', p)
                elif t in (8, 27, 52):
                    v = p[0]
                else:
                    v = p.hex()
                n[t] += 1
                if n[t] <= 8 or t not in (1,):
                    print('%-22s ts=%.3f %s' % (NAMES[t], ts / 1e9, v), flush=True)
finally:
    for fd, t in fds.items():
        w('/sys/class/sensors/ssp_sensor/enable', str(t * 10 + 0))
        d = iio_dev(NAMES[t])
        w(d + '/buffer/enable', '0')
        os.close(fd)
    print('counts', n)
