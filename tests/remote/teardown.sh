#!/bin/sh
# teardown.sh: everything setup.sh started, found by its environment (never by a command-line
# pattern, which would match this script's own SSH command), then a count of what is left.
T=/tmp/s24rt
# (root's processes can't be read: the shell's own complaint about that is hidden too)
onbus() { { tr '\0' '\n' </proc/$1/environ; } 2>/dev/null | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus"; }
for p in $(pgrep -x s24-remote); do onbus $p && kill $p; done
sleep 4
for p in $(pgrep -x kwin_wayland); do onbus $p && kill $p; done
for p in $(pgrep .); do [ "$p" = "$$" ] && continue; onbus $p && kill $p 2>/dev/null; done
[ -f $T/dbus.pid ] && kill $(cat $T/dbus.pid) 2>/dev/null
sleep 1
n=0
for p in $(pgrep .); do onbus $p && n=$((n + 1)); done
echo "left: $n processes on the test bus"
