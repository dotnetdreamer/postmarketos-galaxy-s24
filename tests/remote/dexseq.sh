#!/bin/sh
# dexseq.sh [ROUNDS]: the window script's sequences in a fresh nested session (dexlab.sh): a window on
# the phone's screen beside the browser's, the phone's screen off and on (as Remote Desktop does when a
# browser connects and the power key gives the phone back), the browser's screen gone, docked mode by
# hand on the phone alone. Prints where the window is and how, after each step.
. /tmp/s24rt/t.sh
alive() { for p in $(pgrep -x kwin_wayland); do { tr "\0" "\n" </proc/$p/environ; } 2>/dev/null | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus" && { echo up; return; }; done; echo DEAD; }
st() { [ "$(alive)" = up ] || { echo "KWIN DEAD"; return; }; dump | grep -i "anim caption" | head -1 | sed "s/.*out=/out=/; s/ normal / /; s/ moveable / /; s/ ACTIVE//"; }
dk() { echo "docked=$(grep -o "true\|false" $T/config/plasmamobilerc | head -1) placement=$(grep -o "Placement\[.i\]=[A-Za-z]*" $T/config/plasma-mobile/kwinrc | cut -d= -f2)"; }
anim() {
    for p in $(pgrep -x python3); do tr "\0" "\n" </proc/$p/cmdline 2>/dev/null | grep -q "^$T/anim.py$" && kill $p; done
    sleep 1
    cl anim python3 $T/anim.py
    sleep 4
}
cat >$T/tophone.js <<'EOF'
// (workspace.sendClientToScreen took the nested KWin down every time: moved by its geometry instead)
const out = workspace.screens.find(s => s.name === "Virtual-0");
for (const w of workspace.windowList()) {
    if (w.caption === "s24rt anim" && out) {
        w.setMaximize(false, false);
        w.frameGeometry = { x: out.geometry.x + 100, y: out.geometry.y + 300, width: 800, height: 600 };
    }
}
EOF
sh $T/dexlab.sh stop >/dev/null 2>&1
rm -rf $T/data/kwin/scripts/convergentwindows
mkdir -p $T/data/kwin/scripts
# ORIGINAL=1: Plasma Mobile's own script, for comparison
[ -n "${ORIGINAL:-}" ] || cp -r $T/convergentwindows $T/data/kwin/scripts/
sh $T/dexlab.sh start >/dev/null
echo "start: $(dk), script loaded: $(q org.kde.KWin /Scripting org.kde.kwin.Scripting.isScriptLoaded convergentwindows)"
anim
echo "phone alone, a window: $(st)"
dock() {
    env -i $client_env kwriteconfig6 --notify --file plasmamobilerc --group General --key convergenceModeEnabled $1
    env -i $client_env QT_QPA_PLATFORM=offscreen plasma-mobile-envmanager --apply-settings >/dev/null 2>&1
    sed -i 's/^TabletMode\[\$i\]=auto/TabletMode[$i]=off/' $T/config/plasma-mobile/kwinrc
    q org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1
    sleep 2.5
}
for r in $(seq 1 ${1:-2}); do
    # The nested KWin (virtual backend, no touchscreen) has died as the screen program connected while
    # it had phone mode's rules, Plasma Mobile's own window script loaded or not. So docked mode goes
    # on first (the screen program finds it on and leaves it so) and off by hand after.
    dock true
    sh $T/dexlab.sh hold >/dev/null
    sleep 3
    [ "$(alive)" = up ] || { echo "[$r] the nested KWin died as the browser's screen was added"; exit 1; }
    kjs s24rt-tophone $T/tophone.js
    sleep 2
    echo "[$r] browser's screen up ($(dk)), window on the phone: $(st)"
    env -i $client_env kscreen-doctor output.Virtual-0.disable >/dev/null 2>&1
    sleep 2.5
    echo "[$r] phone's screen off: $(st)"
    env -i $client_env kscreen-doctor output.Virtual-0.enable >/dev/null 2>&1
    sleep 2.5
    echo "[$r] phone's screen on: $(st)"
    kjs s24rt-tophone $T/tophone.js
    sleep 2
    echo "[$r] window sent back to the phone: $(st)"
    kjs s24rt-place $T/place.js
    sleep 2
    echo "[$r] window sent to the browser's screen: $(st)"
    sh $T/dexlab.sh unhold >/dev/null
    sleep 3
    echo "[$r] browser's screen gone ($(dk)): $(st)"
    dock false
    echo "[$r] docked mode off ($(dk)): $(st)"
done
dock true
echo "docked mode by hand, phone alone ($(dk)): $(st)"
dock false
echo "docked mode off by hand ($(dk)): $(st)"
echo "script errors: $(grep -c -E "TypeError|ReferenceError|convergentwindows.*rror" $T/kwin.log)"
