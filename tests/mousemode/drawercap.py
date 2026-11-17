#!/usr/bin/python3
# Open the Plasma Mobile action drawer, capture DSI-1 via KWin ScreenShot2, close the drawer.
# Writes /tmp/qs.raw + /tmp/qs.meta. Run via insess.py (needs the session bus). Claude 2026-11-15.
import dbus, os, threading, time

bus = dbus.SessionBus()
shell = dbus.Interface(bus.get_object('org.kde.plasmashell', '/Mobile'), 'org.kde.plasmashell')
shot = dbus.Interface(bus.get_object('org.kde.KWin', '/org/kde/KWin/ScreenShot2'), 'org.kde.KWin.ScreenShot2')

shell.openActionDrawer()
time.sleep(1.3)

r, w = os.pipe()
chunks = []
def reader():
    while True:
        b = os.read(r, 1 << 16)
        if not b:
            break
        chunks.append(b)
t = threading.Thread(target=reader)
t.start()
try:
    meta = shot.CaptureScreen('DSI-1', dbus.Dictionary({'include-cursor': dbus.Boolean(False)}, signature='sv'),
                              dbus.types.UnixFd(w))
finally:
    os.close(w)
    t.join()
    time.sleep(0.2)
    shell.closeActionDrawer()

raw = b''.join(chunks)
m = {str(k): int(v) if not isinstance(v, dbus.String) else str(v) for k, v in meta.items()}
open('/tmp/qs.raw', 'wb').write(raw)
open('/tmp/qs.meta', 'w').write(repr(m))
print('meta', m, 'bytes', len(raw))
