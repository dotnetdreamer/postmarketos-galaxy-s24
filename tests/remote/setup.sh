#!/bin/sh
# setup.sh: the private nested session (bus, KWin with a phone-sized screen, Plasma Mobile's shell,
# a probe receiver) and s24-remote serving it through a real quick tunnel.
set -u
. /tmp/s24rt/t.sh
mkdir -p $T/config $T/data $T/cache $T/xdg
cp -r /home/user/.config/plasma-mobile $T/xdg/
printf '[General]\nconvergenceModeEnabled=false\n\n[InitialStart]\nwizardRun=true\n' >$T/config/plasmamobilerc
cat >$T/remote.conf <<CONF
# s24-remote, for the nested test session
port = 7181
tunnel = ${S24RT_TUNNEL:-quick}
phone_screen = on
grace_minutes = 1
helper = /usr/local/libexec/s24-remote/openandroiddex-pmos
CONF
env -i $base_env dbus-daemon --config-file=$T/bus.conf --fork --print-pid >$T/dbus.pid
env -i $base_env KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 \
    setsid kwin_wayland --virtual --socket s24rt-0 --width 1080 --height 2340 --scale 2.75 \
    --no-lockscreen --no-global-shortcuts --no-kactivities </dev/null >$T/kwin.log 2>&1 &
for i in $(seq 1 50); do [ -S /run/user/10000/s24rt-0 ] && break; sleep 0.2; done
cl receiver python3 $T/receiver.py $T/kwinprobe.log 7200
cl kamd /usr/lib/libexec/kactivitymanagerd
sleep 2
cl plasmashell plasmashell -p org.kde.plasma.mobileshell
sleep 4
cl remote env S24_REMOTE_CONFIG=$T/remote.conf /usr/local/bin/s24-remote serve
for i in $(seq 1 60); do grep -q '"url":"http' /run/user/10000/s24-remote/status.json 2>/dev/null && break; sleep 1; done
cat /run/user/10000/s24-remote/status.json; echo
echo setup done
