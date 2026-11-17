#!/usr/bin/env python3
# Run a command on the phone over SSH and print its output (exit status = the command's).
#   usage: s24ssh.py "cmd" [timeout-seconds]          settings: see s24common.py (S24_HOST, ...)
# As root, the way the docs do it:
#   s24ssh.py "echo $S24_PASS | sudo -S -p '' sh /home/user/script.sh"
#   (your PC shell expands $S24_PASS before sending; the phone's environment has no such variable)
import sys

from s24common import connect

c, _ = connect()
_, o, e = c.exec_command(sys.argv[1], timeout=float(sys.argv[2]) if len(sys.argv) > 2 else 60)
# raw bytes, so non-ASCII output never trips the console codec
sys.stdout.buffer.write(o.read()); sys.stdout.flush()
sys.stderr.buffer.write(e.read()); sys.stderr.flush()
rc = o.channel.recv_exit_status()
c.close()
sys.exit(rc)
