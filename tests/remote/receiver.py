#!/usr/bin/env python3
# Test receiver (Claude, 2026-11-16): owns org.s24test.Probe on the session bus; Log(s) lines go to a file.
import sys, time
from gi.repository import Gio, GLib
OUT = sys.argv[1]
XML = """<node><interface name='org.s24test.Probe'><method name='Log'><arg type='s' name='text' direction='in'/></method></interface></node>"""
node = Gio.DBusNodeInfo.new_for_xml(XML)
def call(conn, sender, path, iface, method, params, inv):
    with open(OUT, "a") as f:
        f.write("%.3f %s\n" % (time.time(), params.unpack()[0]))
    inv.return_value(None)
def acquired(conn, name):
    conn.register_object("/", node.interfaces[0], call, None, None)
Gio.bus_own_name(Gio.BusType.SESSION, "org.s24test.Probe", Gio.BusNameOwnerFlags.NONE, acquired, None, None)
GLib.timeout_add_seconds(int(sys.argv[2]) if len(sys.argv) > 2 else 600, lambda: loop.quit())
loop = GLib.MainLoop(); loop.run()
