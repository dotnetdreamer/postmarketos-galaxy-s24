#!/bin/sh
# lockonstart.sh with|without: a nested KWin with its screen locker, started the way the automatic
# login starts the desktop (with) or the way a login at the login screen does (without), and whether
# it is locked a few seconds later with nothing asking it to lock. The automatic login's session
# must be locked from its start: kscreenlocker's LockOnStart, from the config folder autologin puts
# in XDG_CONFIG_DIRS (after Plasma Mobile's own folders, as startplasmamobile appends what it finds).
. /tmp/s24rt/t.sh
dirs="$T/config/plasma-mobile:/etc/xdg"
[ "$1" = with ] && dirs="$dirs:/usr/local/share/s24-remote/autologin-xdg"
sh $T/teardown.sh >/dev/null 2>&1
rm -f /run/user/10000/s24rt-0 /run/user/10000/s24rt-0.lock
mkdir -p $T/config $T/data $T/cache
rm -rf $T/config/plasma-mobile; cp -r /home/user/.config/plasma-mobile $T/config/
# the user's own kscreenlockerrc (Autolock off, as on this phone): what the cascade starts from
cp /home/user/.config/kscreenlockerrc $T/config/ 2>/dev/null
env -i $base_env dbus-daemon --config-file=$T/bus.conf --fork --print-pid >$T/dbus.pid
env -i $base_env XDG_CONFIG_DIRS="$dirs" KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 \
    setsid kwin_wayland --virtual --socket s24rt-0 --width 1080 --height 2340 --scale 2.75 \
    --no-global-shortcuts --no-kactivities </dev/null >$T/kwin.log 2>&1 &
for i in $(seq 1 50); do [ -S /run/user/10000/s24rt-0 ] && break; sleep 0.2; done
sleep 6
echo "$1 the autologin folder: locked=$(q org.freedesktop.ScreenSaver /ScreenSaver org.freedesktop.ScreenSaver.GetActive 2>&1)"
sh $T/teardown.sh >/dev/null 2>&1
