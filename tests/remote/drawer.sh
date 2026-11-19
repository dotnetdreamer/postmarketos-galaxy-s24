#!/bin/sh
# drawer.sh off|on: a nested session (no tunnel, s24-remote's runtime files private), its action drawer
# opened over D-Bus, and a picture of the phone-sized screen, with Remote Desktop off or on.
. /tmp/s24rt/t.sh
mkdir -p $T/config $T/data $T/cache $T/xdg $T/run
chmod 700 $T/run
if ! [ -S /run/user/10000/s24rt-0 ]; then
    cp -r /home/user/.config/plasma-mobile $T/xdg/
    printf '[General]\nconvergenceModeEnabled=false\n\n[InitialStart]\nwizardRun=true\n' >$T/config/plasmamobilerc
    printf 'port = 7182\ntunnel = none\n' >$T/drawer.conf
    env -i $base_env dbus-daemon --config-file=$T/bus.conf --fork --print-pid >$T/dbus.pid
    env -i $base_env KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 \
        setsid kwin_wayland --virtual --socket s24rt-0 --width 1080 --height 2340 --scale 2.75 \
        --no-lockscreen --no-global-shortcuts --no-kactivities </dev/null >$T/kwin.log 2>&1 &
    for i in $(seq 1 50); do [ -S /run/user/10000/s24rt-0 ] && break; sleep 0.2; done
    cl kamd /usr/lib/libexec/kactivitymanagerd
    sleep 2
    cl plasmashell plasmashell -p org.kde.plasma.mobileshell
    sleep 8
fi
if [ "$1" = on ]; then
    cl remote env S24_REMOTE_CONFIG=$T/drawer.conf XDG_RUNTIME_DIR=$T/run /usr/local/bin/s24-remote serve
    sleep 4
    cat $T/run/s24-remote/status.json; echo
fi
q org.kde.plasmashell /Mobile org.kde.plasmashell.openActionDrawer
sleep 2
shot Virtual-0 drawer-$1
q org.kde.plasmashell /Mobile org.kde.plasmashell.closeActionDrawer
