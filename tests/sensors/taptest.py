#!/usr/bin/python3
# Double-tap test (Claude, 2026-11-15): "off" = a power key press like the user's (screen off),
# "tap" = KEY_WAKEUP written into the touch chip's own input device, exactly what its double-tap
# gesture sends; then time until the panel lights, and look again 2 s later (pocket check).
import os, sys, time, struct, importlib.machinery, importlib.util
loader = importlib.machinery.SourceFileLoader('powerd', '/usr/local/sbin/s24-powerd')
spec = importlib.util.spec_from_loader('powerd', loader)
pd = importlib.util.module_from_spec(spec)
loader.exec_module(pd)
drm, conn = pd.drm_state_file(), pd.dsi_enabled_file()
lit = lambda: pd.panel_lit(drm, conn)

def press(dev, code):
    fd = os.open(dev, os.O_WRONLY)
    try:
        os.write(fd, struct.pack('<qqHHi', 0, 0, 1, code, 1) + struct.pack('<qqHHi', 0, 0, 0, 0, 0))
        time.sleep(0.05)
        os.write(fd, struct.pack('<qqHHi', 0, 0, 1, code, 0) + struct.pack('<qqHHi', 0, 0, 0, 0, 0))
    finally:
        os.close(fd)

mode = sys.argv[1]
if mode == 'off':
    if not lit():
        print('already dark'); sys.exit(0)
    t0 = time.monotonic(); press(pd.find_input('s2mps27-power-keys'), pd.KEY_POWER)
    while lit() and time.monotonic() - t0 < 5:
        time.sleep(0.02)
    print('dark after %.2f s' % (time.monotonic() - t0) if not lit() else 'STILL LIT after 5 s')
elif mode == 'tap':
    if lit():
        print('screen is lit: no test'); sys.exit(1)
    t0 = time.monotonic(); press(pd.find_input('sec_touchscreen'), pd.KEY_WAKEUP)
    while not lit() and time.monotonic() - t0 < 6:
        time.sleep(0.01)
    print('lit after %.2f s' % (time.monotonic() - t0) if lit() else 'NOT LIT after 6 s')
    time.sleep(2)
    print('2 s later:', 'lit' if lit() else 'dark')
