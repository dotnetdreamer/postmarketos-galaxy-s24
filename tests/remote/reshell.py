#!/usr/bin/env python3
# reshell.py OLD_PID: start plasmashell --replace with OLD_PID's exact environment (a dict: the
# session's LD_PRELOAD holds a space), in a new session, logging to /tmp/s24rt/reshell.log.
import os, subprocess, sys
pid = sys.argv[1]
env = {}
for item in open("/proc/%s/environ" % pid, "rb").read().split(b"\0"):
    if b"=" in item:
        k, v = item.split(b"=", 1)
        env[k.decode()] = v.decode(errors="replace")
log = open("/tmp/s24rt/reshell.log", "ab")
p = subprocess.Popen(["/usr/bin/plasmashell", "--replace"], env=env, cwd=env.get("HOME", "/"),
                     stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
print("started plasmashell", p.pid)
