#!/usr/bin/env python3
# Push a local file to the phone by piping it into `cat > REMOTE` (dropbear has no SFTP).
#   usage: s24put.py LOCAL REMOTE                     settings: see s24common.py
import hashlib
import sys

from s24common import connect

local, remote = sys.argv[1], sys.argv[2]
data = open(local, "rb").read()
c, _ = connect()
chan = c.get_transport().open_session()
chan.exec_command("cat > '%s'" % remote)
chan.sendall(data)
chan.shutdown_write()
rc = chan.recv_exit_status()
err = chan.recv_stderr(65536).decode(errors="replace")
c.close()
print("%s -> %s (%d bytes, md5 %s) rc=%s %s" % (local, remote, len(data), hashlib.md5(data).hexdigest(), rc, err.strip()))
sys.exit(rc)
