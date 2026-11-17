#!/bin/sh
# per-thread CPU of the camera provider over 5 s (to find idle burners)
unset LD_PRELOAD LD_LIBRARY_PATH
PP=$(pgrep -f camera.provider-service_64|head -1)
for t in /proc/$PP/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)"; done > /tmp/t0
sleep 5
for t in /proc/$PP/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)"; done > /tmp/t1
echo "provider $PP threads: $(ls /proc/$PP/task | wc -l); top CPU threads over 5 s (ticks, 100/s):"
awk 'NR==FNR{a[$1]=$2;next}{d=$2-a[$1]; if(d>0) print d, $1, $3}' /tmp/t0 /tmp/t1 | sort -rn | head -12
echo "s24-cam running: $(pgrep -f 's24-cam[^ ]* (stream|serve)'|head -1); pw state: $(grep '^\[pw\]' /tmp/bridge.log | tail -1)"
