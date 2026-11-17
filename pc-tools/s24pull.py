#!/usr/bin/env python3
# Pull a (possibly binary, possibly root-only) file from the phone: sudo cat over an exec channel.
#   usage: s24pull.py REMOTE LOCAL                     settings: see s24common.py
import sys

from s24common import connect

remote, local = sys.argv[1], sys.argv[2]
c, pw = connect()
chan = c.get_transport().open_session()
chan.exec_command("sudo -S -p '' cat '%s' 2>/dev/null" % remote)
chan.sendall((pw + "\n").encode())
data = b""
while True:
    d = chan.recv(1 << 20)
    if not d:
        break
    data += d
chan.recv_exit_status()
c.close()
open(local, "wb").write(data)
print("%s -> %s (%d bytes)" % (remote, local, len(data)))
