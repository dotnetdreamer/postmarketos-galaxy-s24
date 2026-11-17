# press the power key once (as s24-powerd does for double-tap wake): KWin toggles the screen
import glob, os, struct, time
for d in glob.glob('/sys/class/input/event*'):
    try:
        if open(d + '/device/name').read().strip() == 's2mps27-power-keys':
            fd = os.open('/dev/input/' + os.path.basename(d), os.O_WRONLY)
            for code, val in ((116, 1), (0, 0), (116, 0), (0, 0)):
                os.write(fd, struct.pack('<qqHHi', 0, 0, 1 if code else 0, code, val))
                if code and val:
                    time.sleep(0.05)
            os.close(fd)
            break
    except OSError:
        pass
