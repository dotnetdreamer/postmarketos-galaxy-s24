#!/bin/sh
# on-demand camera: closes when no app watches, reopens when one connects; idle CPU per round
unset LD_PRELOAD LD_LIBRARY_PATH
echo keep > /sys/power/wake_lock 2>/dev/null
PP=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1); CD=$(pgrep -f '^/root/s24-camd'|head -1); SP=$(pgrep -f 's24-cam[^ ]* (stream|serve)'|head -1)
t(){ awk '{print $14+$15}' /proc/$1/stat 2>/dev/null; }
for round in 1 2 3; do
  sleep 8   # the idle close happens 5 s after the app leaves
  a=$(t $PP); b=$(t $CD); c=$(t $SP); sleep 10
  echo "round $round, camera idle 10 s, CPU ticks (100 = 1 core-second): provider $(( $(t $PP) - a )), s24-camd $(( $(t $CD) - b )), s24-cam $(( $(t $SP) - c )) | $(grep -E 'camera closed' /tmp/bridge.log | tail -1)"
  su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_rear 6" 2>&1 | grep -E 'first frame|frames,' | sed 's/^/   /'
  grep -E 'camera ready|HAL default template' /tmp/bridge.log | tail -2 | sed 's/^/   /'
done
echo "alive: provider=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1) (was $PP) s24-cam=$(pgrep -f 's24-cam[^ ]* serve'|head -1) (was $SP)"
grep -E 'queue' /tmp/s24-camd.log | tail -14
