#!/bin/sh
# anim.sh: (re)open the animating window in the nested session, on the browser's screen.
. /tmp/s24rt/t.sh
for p in $(pgrep -x python3); do tr '\0' '\n' </proc/$p/cmdline 2>/dev/null | grep -q "^$T/anim.py$" && kill $p; done
sleep 1
cl anim python3 $T/anim.py
sleep 3
kjs s24rt-place $T/place.js
sleep 1
kjs s24rt-place $T/place.js
sleep 1
dump | grep -i anim | head -1
