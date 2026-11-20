#!/bin/sh
# dexlab.sh start|hold|unhold|panels|addpanels|prio SCREEN|dump|script|stop: the phone's panel as a
# phone beside the browser's screen (Remote Desktop's DeX-like mode), tried in a private nested
# session: its own bus and KWin (a phone-sized virtual screen named Virtual-0 standing in for DSI-1),
# Plasma Mobile's shell, and the screen program's browser screen held up with no browser. No
# s24-remote here: a second one would share the real one's files in $XDG_RUNTIME_DIR.
. /tmp/s24rt/t.sh
# Phone mode's KWin rules have TabletMode=auto. This KWin has no touchscreen, and with "auto" it has died
# as the screen program (which adds a fake pointer and keyboard) connected: off here, after every
# rewrite of the rules (docked mode has it off anyway). A phone has its touchscreen; not seen there.
notablet() {
    sed -i 's/^TabletMode\[\$i\]=auto/TabletMode[$i]=off/' $T/config/plasma-mobile/kwinrc 2>/dev/null
    q org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1
}
case "$1" in
start)
    mkdir -p $T/config $T/data/kwin/scripts $T/cache $T/xdg
    rm -rf $T/config/plasma-mobile; cp -r /home/user/.config/plasma-mobile $T/config/
    printf '[General]\nconvergenceModeEnabled=false\n\n[InitialStart]\nwizardRun=true\n' >$T/config/plasmamobilerc
    # Virtual-0 is the "phone's panel" for the window script under test
    printf '[Script-convergentwindows]\nphoneScreen=Virtual-0\n' >$T/config/kwinrc
    # a socket left by an earlier test KWin would pass the wait below before the new one listens
    rm -f /run/user/10000/s24rt-0 /run/user/10000/s24rt-0.lock
    env -i $base_env dbus-daemon --config-file=$T/bus.conf --fork --print-pid >$T/dbus.pid
    env -i $base_env KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 \
        setsid kwin_wayland --virtual --socket s24rt-0 --width 1080 --height 2340 --scale 2.75 \
        --no-lockscreen --no-global-shortcuts --no-kactivities </dev/null >$T/kwin.log 2>&1 &
    for i in $(seq 1 50); do [ -S /run/user/10000/s24rt-0 ] && break; sleep 0.2; done
    # KWin's rules for phone mode, as startplasmamobile applies them (the copy above may be docked's)
    env -i $client_env QT_QPA_PLATFORM=offscreen plasma-mobile-envmanager --apply-settings >/dev/null 2>&1
    notablet
    cl receiver python3 $T/receiver.py $T/kwinprobe.log 3600
    cl kamd /usr/lib/libexec/kactivitymanagerd
    for i in $(seq 1 50); do q org.kde.ActivityManager / >/dev/null 2>&1 && break; sleep 0.2; done
    cl plasmashell plasmashell -p org.kde.plasma.mobileshell
    for i in $(seq 1 75); do q org.kde.plasmashell /PlasmaShell >/dev/null 2>&1 && break; sleep 0.2; done
    sleep 3
    echo started
    ;;
hold) sh $T/holdscreen.sh start ;;
unhold) sh $T/holdscreen.sh stop; notablet ;;
panels)
    q org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript '
var s = "screens: " + screenCount + "\n";
for (var i = 0; i < screenCount; i++) { var g = screenGeometry(i); s += "  screen " + i + ": " + g.x + "," + g.y + " " + g.width + "x" + g.height + "\n"; }
var p = panels();
for (var i = 0; i < p.length; i++) { p[i].currentConfigGroup = ["S24Remote"]; s += "panel " + p[i].type + " id " + p[i].id + " screen " + p[i].screen + " location " + p[i].location + " ours " + p[i].readConfig("secondScreen", false) + "\n"; }
print(s);'
    ;;
addpanels)
    q org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript "$(cat $T/panels2.js)"
    ;;
prio)
    env -i $client_env kscreen-doctor "output.$2.priority.1" 2>&1 | tail -3
    env -i $client_env kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -E "^Output|priority"
    ;;
dump) dump ;;
script)
    # the window script under test, in place of Plasma Mobile's: unloaded, then KWin's reconfigure
    # loads the enabled scripts again, finding ours first ($XDG_DATA_HOME)
    rm -rf $T/data/kwin/scripts/convergentwindows
    cp -r $T/convergentwindows $T/data/kwin/scripts/
    q org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript convergentwindows
    q org.kde.KWin /KWin org.kde.KWin.reconfigure
    sleep 1
    q org.kde.KWin /Scripting org.kde.kwin.Scripting.isScriptLoaded convergentwindows
    grep -i -E "convergentwindows|qml|script" $T/kwin.log | tail -5
    ;;
stop)
    sh $T/holdscreen.sh stop
    sh $T/teardown.sh
    ;;
esac
