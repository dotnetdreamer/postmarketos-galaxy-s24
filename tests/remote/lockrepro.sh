#!/bin/sh
# lockrepro.sh lock|screen|panel|log: a nested KWin WITH its screen locker (Plasma Mobile's lock
# screen, as the real session has it), locked; then screens added and removed the way Remote Desktop
# does, watching the greeter (the lock screen's own process) in KWin's log.
. /tmp/s24rt/t.sh
mkdir -p $T/config $T/data $T/cache $T/xdg
kwinpid() {
    for p in $(pgrep -x kwin_wayland); do
        { tr '\0' '\n' </proc/$p/environ; } 2>/dev/null | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus" && echo $p
    done | head -1
}
greeter() { k=$(kwinpid); ps -o pid,ppid,args | awk -v k="$k" '$2 == k && /kscreenlocker_greet/ {print $1}' | head -1; }
crashes() { grep -c "exit unregular" $T/kwin.log; }
if ! [ -S /run/user/10000/s24rt-0 ]; then
    cp -r /home/user/.config/plasma-mobile $T/xdg/
    for f in kscreenlockerrc kdeglobals plasmashellrc; do cp /home/user/.config/$f $T/config/ 2>/dev/null; done
    printf '[General]\nconvergenceModeEnabled=false\n\n[InitialStart]\nwizardRun=true\n' >$T/config/plasmamobilerc
    env -i $base_env dbus-daemon --config-file=$T/bus.conf --fork --print-pid >$T/dbus.pid
    env -i $base_env KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 \
        PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell QT_QUICK_CONTROLS_MOBILE=true \
        QT_QUICK_CONTROLS_STYLE=org.kde.breeze QT_QPA_PLATFORMTHEME=KDE \
        setsid kwin_wayland --virtual --socket s24rt-0 --width 1080 --height 2340 --scale 2.75 \
        --no-global-shortcuts --no-kactivities </dev/null >$T/kwin.log 2>&1 &
    for i in $(seq 1 50); do [ -S /run/user/10000/s24rt-0 ] && break; sleep 0.2; done
    cl kamd /usr/lib/libexec/kactivitymanagerd
    sleep 2
    cl plasmashell plasmashell -p org.kde.plasma.mobileshell
    sleep 8
fi
case "$1" in
lock)
    q org.freedesktop.ScreenSaver /ScreenSaver org.freedesktop.ScreenSaver.Lock
    sleep 5
    echo "locked: $(q org.freedesktop.ScreenSaver /ScreenSaver org.freedesktop.ScreenSaver.GetActive), greeter: $(greeter), crashes: $(crashes)"
    grep -o "/usr/share/plasma/[a-z/.-]*lockscreen/[A-Za-z]*.qml" $T/kwin.log | sort -u | head -2
    ;;
screen)
    # one browser's screen, made and put away, as the helper does (its snapshot command)
    env -i $client_env /usr/local/libexec/s24-remote/openandroiddex-pmos snapshot --width 1600 --height 900 --scale 1 \
        --out $T/snap.png --wait 2 >>$T/snap.log 2>&1
    sleep 2
    echo "after a screen: greeter $(greeter), crashes $(crashes)"
    ;;
panel)
    # the phone-sized screen off and on again, as Remote Desktop does with the panel
    env -i $client_env kscreen-doctor output.Virtual-0.disable >>$T/snap.log 2>&1
    sleep 2
    env -i $client_env kscreen-doctor output.Virtual-0.enable >>$T/snap.log 2>&1
    sleep 2
    echo "after panel off/on: greeter $(greeter), crashes $(crashes)"
    ;;
log)
    grep -iE "greeter|unregular|emergency|software rendering|segfault|signal" $T/kwin.log | tail -${2:-15}
    ;;
esac
