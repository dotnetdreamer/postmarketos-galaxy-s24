#!/usr/bin/env python3
"""Checks s24-dockd's rules without a monitor and without touching the real settings.

Runs as the normal user on the phone (or anywhere with python3 + PyGObject). It loads
/usr/local/bin/s24-dockd (or the path given), points HOME at a temporary folder, puts stand-in
kreadconfig6 / kwriteconfig6 / plasma-mobile-envmanager programs first in PATH (they keep the
setting in a file), and fakes "monitor connected" instead of reading /sys.
"""
import importlib.machinery
import importlib.util
import os
import sys
import tempfile

DOCKD = sys.argv[1] if len(sys.argv) > 1 else "/usr/local/bin/s24-dockd"

tmp = tempfile.mkdtemp(prefix="dockd-test-")
bin_dir = os.path.join(tmp, "bin")
os.makedirs(bin_dir)
setting = os.path.join(tmp, "convergenceModeEnabled")
envmanager_runs = os.path.join(tmp, "envmanager-runs")
stubs = {
    "kreadconfig6": '#!/bin/sh\ncat "%s" 2>/dev/null || echo false\n' % setting,
    # the value is the last argument
    "kwriteconfig6": '#!/bin/sh\nfor v; do :; done\necho "$v" > "%s"\n' % setting,
    "plasma-mobile-envmanager": '#!/bin/sh\necho run >> "%s"\n' % envmanager_runs,
}
for name, body in stubs.items():
    path = os.path.join(bin_dir, name)
    with open(path, "w") as f:
        f.write(body)
    os.chmod(path, 0o755)
os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
os.environ["HOME"] = tmp
os.environ["XDG_CACHE_HOME"] = os.path.join(tmp, "cache")

loader = importlib.machinery.SourceFileLoader("s24_dockd", DOCKD)
spec = importlib.util.spec_from_loader("s24_dockd", loader)
d = importlib.util.module_from_spec(spec)
loader.exec_module(d)

monitor = [False]
d.monitor_connected = lambda: monitor[0]


def value():
    try:
        return open(setting).read().strip()
    except OSError:
        return "false"


def runs():
    try:
        return len(open(envmanager_runs).read().split())
    except OSError:
        return 0


def reset(phone_setting, stale_before=None):
    for p in (setting, envmanager_runs, d.BEFORE):
        if os.path.exists(p):
            os.unlink(p)
    if phone_setting is not None:
        with open(setting, "w") as f:
            f.write(phone_setting + "\n")
    if stale_before is not None:
        os.makedirs(d.STATE, exist_ok=True)
        with open(d.BEFORE, "w") as f:
            f.write(stale_before + "\n")


failures = 0


def check(name, cond):
    global failures
    print(("ok   " if cond else "FAIL ") + name)
    failures += 0 if cond else 1


# 1. phone had it off: plug in -> on (rules applied), kept "false"; unplug -> off again, file gone
reset("false")
monitor[0] = True
d.sync()
check("plug in: docked mode on", value() == "true")
check("plug in: KWin rules applied", runs() == 1)
check("plug in: what the phone had is kept", open(d.BEFORE).read().strip() == "false")
d.sync()
check("second check while plugged: nothing rewritten", runs() == 1)
monitor[0] = False
d.sync()
check("unplug: docked mode back off", value() == "false")
check("unplug: KWin rules applied again", runs() == 2)
check("unplug: kept file removed", not os.path.exists(d.BEFORE))
d.sync()
check("second check while unplugged: nothing rewritten", runs() == 2)

# 2. phone had it on by choice: left alone both ways
reset("true")
monitor[0] = True
d.sync()
check("on by choice, plug in: still on, nothing kept", value() == "true" and not os.path.exists(d.BEFORE))
monitor[0] = False
d.sync()
check("on by choice, unplug: still on", value() == "true" and runs() == 0)

# 3. restart while docked, no monitor at the next login: put back off
reset("true", stale_before="false")
monitor[0] = False
d.sync()
check("stale kept file, no monitor: back off", value() == "false" and not os.path.exists(d.BEFORE))

# 4. restart while docked, monitor still there: stays on, file kept for the unplug
reset("true", stale_before="false")
monitor[0] = True
d.sync()
check("stale kept file, monitor there: stays on", value() == "true" and os.path.exists(d.BEFORE))
monitor[0] = False
d.sync()
check("then unplug: back off", value() == "false")

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
