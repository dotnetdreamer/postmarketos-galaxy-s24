#!/bin/sh
# probe.sh: (re)open the probe window in the nested session, then show where KWin put it.
. /tmp/s24rt/t.sh
for p in $(pgrep -x python3); do tr '\0' '\n' </proc/$p/cmdline 2>/dev/null | grep -q "^$T/probe.py$" && kill $p; done
sleep 1
: >$T/probe.log
cl probe python3 $T/probe.py $T/probe.log
sleep 4
dump
