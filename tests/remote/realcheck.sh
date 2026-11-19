#!/bin/sh
# realcheck.sh: the real session's state, read only: lock, screens, sleep locks, power log, remote status.
. /tmp/s24rt/real.sh
echo "locked: $(gdbus call --session -d org.freedesktop.ScreenSaver -o /ScreenSaver -m org.freedesktop.ScreenSaver.GetActive 2>&1)"
QT_QPA_PLATFORM=wayland kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^Output|^	enabled|^	disabled|Geometry" | head -12
echo "inhibitors:"; elogind-inhibit --list --mode=block 2>/dev/null | tail -n +2 | grep -v "^$" | head -8
echo "power log:"; tail -3 /var/log/s24-power.log 2>&1 | cut -c1-150
echo "remote: $(/usr/local/bin/s24-remote status 2>&1 | head -1)"
