#!/usr/bin/env python3
"""Checks s24-dockd's rules without a monitor and without touching the real settings or sound.

Runs as the normal user on the phone (or anywhere with python3 + PyGObject). It loads
/usr/local/bin/s24-dockd (or the path given), points HOME at a temporary folder, puts stand-in
kreadconfig6 / kwriteconfig6 / plasma-mobile-envmanager / pactl programs first in PATH (they
keep their state in files there), and fakes the monitor instead of reading /sys.
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile

DOCKD = sys.argv[1] if len(sys.argv) > 1 else "/usr/local/bin/s24-dockd"

tmp = tempfile.mkdtemp(prefix="dockd-test-")
bin_dir = os.path.join(tmp, "bin")
os.makedirs(bin_dir)
setting = os.path.join(tmp, "convergenceModeEnabled")
envmanager_runs = os.path.join(tmp, "envmanager-runs")
pulse = os.path.join(tmp, "pulse.json")

PACTL = r'''#!/usr/bin/env python3
# a stand-in PulseAudio for pactl's few commands s24-dockd uses
import json, sys
p = %r
s = json.load(open(p))
a = sys.argv[1:]
if not s["up"]:
    sys.exit(1)
def save():
    json.dump(s, open(p, "w"))
if a[:3] == ["list", "short", "modules"]:
    for i, m in sorted(s["modules"].items()):
        print("%%s\t%%s\t%%s" %% (i, m["name"], m["args"]))
elif a[:3] == ["list", "short", "sinks"]:
    for name, mod in s["sinks"].items():
        print("%%s\t%%s\tmodule-alsa-sink.c\ts16le 2ch 48000Hz\tIDLE" %% (mod, name))
elif a[:3] == ["list", "short", "sink-inputs"]:
    for i, sink in s["inputs"].items():
        print("%%s\t%%s\t-\tprotocol-native.c\ts16le 2ch 48000Hz" %% (i, sink))
elif a[0] == "load-module":
    if s["fail_load"]:
        sys.exit(1)
    args = " ".join(a[2:])
    i = str(max([int(k) for k in s["modules"]] + [0]) + 1)
    s["modules"][i] = {"name": a[1], "args": args}
    name = [w for w in args.split() if w.startswith("sink_name=")][0].split("=", 1)[1]
    s["sinks"][name] = i
    s["loads"] += 1
    s["default"] = name  # module-switch-on-connect (postmarketOS loads it) moves the default at once
    save()
    print(i)
elif a[0] == "unload-module":
    m = s["modules"].pop(a[1])
    gone = [n for n, mod in s["sinks"].items() if mod == a[1]]
    for n in gone:
        del s["sinks"][n]
    if s["default"] in gone:
        s["default"] = next(iter(s["sinks"]), "")
    for i, sink in s["inputs"].items():
        if sink in gone:
            s["inputs"][i] = s["default"]
    save()
elif a[0] == "get-default-sink":
    print(s["default"])
elif a[0] == "set-default-sink":
    if a[1] not in s["sinks"]:
        sys.exit(1)
    s["default"] = a[1]
    save()
elif a[0] == "move-sink-input":
    s["inputs"][a[1]] = a[2]
    save()
else:
    sys.exit(1)
''' % pulse

stubs = {
    "kreadconfig6": '#!/bin/sh\ncat "%s" 2>/dev/null || echo false\n' % setting,
    "kwriteconfig6": '#!/bin/sh\nfor v; do :; done\necho "$v" > "%s"\n' % setting,  # the value is the last argument
    "plasma-mobile-envmanager": '#!/bin/sh\necho run >> "%s"\n' % envmanager_runs,
    "pactl": PACTL,
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

monitor = {"connected": False, "audio": True}
d.monitor_connected = lambda: monitor["connected"]
d.monitor_has_audio = lambda: monitor["audio"]
d.monitor_name = lambda: "TestMonitor"


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


def pa():
    return json.load(open(pulse))


def pa_reset(default="s24_speaker", extra_sinks=(), up=True, fail_load=False):
    s = {"up": up, "fail_load": fail_load, "loads": 0, "default": default,
         "modules": {"1": {"name": "module-alsa-sink", "args": "device=hw:0,100 sink_name=s24_speaker"}},
         "sinks": {"s24_speaker": "1"}, "inputs": {"7": default}}
    for n, name in enumerate(extra_sinks, 2):
        s["modules"][str(n)] = {"name": "module-bluez5-device", "args": "path=/x"}
        s["sinks"][name] = str(n)
    json.dump(s, open(pulse, "w"))


def pa_set(**kw):
    s = pa()
    s.update(kw)
    json.dump(s, open(pulse, "w"))


def reset(phone_setting, stale_before=None):
    for p in (setting, envmanager_runs, d.BEFORE, d.SINK_BEFORE):
        if os.path.exists(p):
            os.unlink(p)
    if phone_setting is not None:
        with open(setting, "w") as f:
            f.write(phone_setting + "\n")
    if stale_before is not None:
        os.makedirs(d.STATE, exist_ok=True)
        with open(d.BEFORE, "w") as f:
            f.write(stale_before + "\n")
    pa_reset()


failures = 0


def check(name, cond):
    global failures
    print(("ok   " if cond else "FAIL ") + name)
    failures += 0 if cond else 1


# ── docked mode ──
# 1. phone had it off: plug in -> on (rules applied), kept "false"; unplug -> off again, file gone
reset("false")
monitor.update(connected=True, audio=False)
d.sync()
check("plug in: docked mode on", value() == "true")
check("plug in: KWin rules applied", runs() == 1)
check("plug in: what the phone had is kept", open(d.BEFORE).read().strip() == "false")
d.sync()
check("second check while plugged: nothing rewritten", runs() == 1)
monitor["connected"] = False
d.sync()
check("unplug: docked mode back off", value() == "false")
check("unplug: KWin rules applied again", runs() == 2)
check("unplug: kept file removed", not os.path.exists(d.BEFORE))
d.sync()
check("second check while unplugged: nothing rewritten", runs() == 2)

# 2. phone had it on by choice: left alone both ways
reset("true")
monitor["connected"] = True
d.sync()
check("on by choice, plug in: still on, nothing kept", value() == "true" and not os.path.exists(d.BEFORE))
monitor["connected"] = False
d.sync()
check("on by choice, unplug: still on", value() == "true" and runs() == 0)

# 3. restart while docked, no monitor at the next login: put back off
reset("true", stale_before="false")
monitor["connected"] = False
d.sync()
check("stale kept file, no monitor: back off", value() == "false" and not os.path.exists(d.BEFORE))

# 4. restart while docked, monitor still there: stays on, file kept for the unplug
reset("true", stale_before="false")
monitor["connected"] = True
d.sync()
check("stale kept file, monitor there: stays on", value() == "true" and os.path.exists(d.BEFORE))
monitor["connected"] = False
d.sync()
check("then unplug: back off", value() == "false")

# ── sound ──
# 5. a monitor with sound: its sink comes, becomes the default, takes the playing stream
reset("false")
monitor.update(connected=True, audio=True)
ok = d.sync()
s = pa()
check("sound: sync reports done", ok is True)
check("sound: display sink added", "s24_display" in s["sinks"])
check("sound: display is the default", s["default"] == "s24_display")
check("sound: playing stream moved to the display", s["inputs"]["7"] == "s24_display")
check("sound: the old default is kept", d.read_file(d.SINK_BEFORE) == "s24_speaker")
d.sync()
check("sound: second check adds nothing", pa()["loads"] == 1)

# 6. the user picks the speaker while docked: left alone
pa_set(default="s24_speaker")
d.sync()
check("sound: user's choice while docked stays", pa()["default"] == "s24_speaker" and pa()["loads"] == 1)

# 7. unplug: sink gone, the old default back, the stream back on the speaker
pa_set(default="s24_display")
monitor["connected"] = False
d.sync()
s = pa()
check("unplug: display sink removed", "s24_display" not in s["sinks"])
check("unplug: default back to the speaker", s["default"] == "s24_speaker")
check("unplug: stream back on the speaker", s["inputs"]["7"] == "s24_speaker")
check("unplug: kept default removed", not os.path.exists(d.SINK_BEFORE))

# 8. headphones were the default before docking: they come back, not the speaker
reset("false")
pa_reset(default="bt_headphones", extra_sinks=("bt_headphones",))
monitor.update(connected=True, audio=True)
d.sync()
monitor["connected"] = False
d.sync()
check("headphones before docking: back to headphones", pa()["default"] == "bt_headphones")

# 9. a monitor without sound: no sink
reset("false")
monitor.update(connected=True, audio=False)
check("no sound in the monitor: sync done", d.sync() is True)
check("no sound in the monitor: no sink", "s24_display" not in pa()["sinks"])

# 10. PulseAudio not up yet, or the monitor's sound not announced yet: retry, then it works
reset("false")
pa_reset(up=False)
monitor.update(connected=True, audio=True)
check("PulseAudio down: asks for a retry", d.sync() is False)
pa_set(up=True, fail_load=True)
check("sound not announced yet: asks for a retry", d.sync() is False)
pa_set(fail_load=False)
check("then: done", d.sync() is True and pa()["default"] == "s24_display")

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
