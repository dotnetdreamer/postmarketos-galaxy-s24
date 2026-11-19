#!/bin/sh
# reshell-nested.sh: plasmashell --replace in the nested session, as was done in the real one at 00:22.
. /tmp/s24rt/t.sh
cl plasmashell2 plasmashell --replace -p org.kde.plasma.mobileshell
sleep 15
k=""; for p in $(pgrep -x kwin_wayland); do { tr '\0' '\n' </proc/$p/environ; } 2>/dev/null | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus" && k=$p; done
echo "greeter: $(ps -o pid,ppid,args | awk -v k="$k" '$2 == k && /kscreenlocker_greet/ {print $1}' | head -1), crashes: $(grep -c 'exit unregular' $T/kwin.log), emergency: $(grep -c 'emergency mode' $T/kwin.log)"
