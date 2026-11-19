#!/usr/bin/env python3
# shot.py SCREEN OUT.ppm: KWin's ScreenShot2 picture of one screen of the nested KWin (Claude, 2026-11-16).
import os, sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
r, w = os.pipe()
fds = Gio.UnixFDList.new()
idx = fds.append(w)
os.close(w)
res, _ = bus.call_with_unix_fd_list_sync("org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
    "CaptureScreen", GLib.Variant("(sa{sv}h)", (sys.argv[1], {"native-resolution": GLib.Variant("b", True)}, idx)),
    GLib.VariantType("(a{sv})"), Gio.DBusCallFlags.NONE, 10000, fds, None)
meta = res.unpack()[0]
del fds
data = b""
while True:
    b = os.read(r, 1 << 20)
    if not b:
        break
    data += b
W, H, S, F = meta["width"], meta["height"], meta["stride"], meta["format"]
out = bytearray()
for y in range(H):
    row = data[y * S:y * S + W * 4]
    if F in (16, 17, 18):            # RGBX / RGBA bytes
        for x in range(0, W * 4, 4):
            out += row[x:x + 3]
    else:                            # ARGB32 in memory: B G R A
        for x in range(0, W * 4, 4):
            out += bytes((row[x + 2], row[x + 1], row[x]))
with open(sys.argv[2], "wb") as f:
    f.write(b"P6 %d %d 255\n" % (W, H) + bytes(out))
print("%s: %dx%d format %d" % (sys.argv[1], W, H, F))
