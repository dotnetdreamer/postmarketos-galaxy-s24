#!/bin/sh
# notetest.sh: Remote Desktop's notifications on a locked nested session: on (starting, then ready
# with the code), then off (the notification closed), three times; the greeter's crashes after each.
. /tmp/s24rt/t.sh
mkdir -p $T/run; chmod 700 $T/run
printf 'port = 7182\ntunnel = none\n' >$T/drawer.conf
k=""; for p in $(pgrep -x kwin_wayland); do { tr '\0' '\n' </proc/$p/environ; } 2>/dev/null | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus" && k=$p; done
for i in 1 2 3; do
    cl remote env S24_REMOTE_CONFIG=$T/drawer.conf XDG_RUNTIME_DIR=$T/run /usr/local/bin/s24-remote serve
    sleep 6
    for p in $(pgrep -x s24-remote); do { tr '\0' '\n' </proc/$p/environ; } 2>/dev/null | grep -q "^S24_REMOTE_CONFIG=$T/drawer.conf" && kill $p; done
    sleep 4
    echo "round $i: greeter $(ps -o pid,ppid,args | awk -v k="$k" '$2 == k && /kscreenlocker_greet/ {print $1}' | head -1), crashes $(grep -c 'exit unregular' $T/kwin.log)"
done
