#!/bin/sh
# flip.sh: (re)open the flip window in the nested session, on the browser's screen, focused.
. /tmp/s24rt/t.sh
for p in $(pgrep -x python3); do tr '\0' '\n' </proc/$p/cmdline 2>/dev/null | grep -q "^$T/flip.py$" && kill $p; done
sleep 1
cl flip python3 $T/flip.py
sleep 3
kjs s24rt-placeflip $T/place-flip.js
sleep 1
kjs s24rt-placeflip $T/place-flip.js
sleep 1
dump | grep -i flip | head -1
