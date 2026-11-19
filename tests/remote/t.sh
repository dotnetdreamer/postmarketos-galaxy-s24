# t.sh: sourced. A private nested Plasma Mobile session for s24-remote's tests (Claude, 2026-11-19).
# Nothing of the user's real session is used: own bus, own KWin (virtual backend, no input devices),
# own config/data/cache dirs. Only XDG_RUNTIME_DIR is shared (PipeWire lives there).
T=/tmp/s24rt
base_env="HOME=/home/user PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin USER=user LANG=C.UTF-8
XDG_RUNTIME_DIR=/run/user/10000 XDG_CONFIG_HOME=$T/config XDG_DATA_HOME=$T/data XDG_CACHE_HOME=$T/cache
XDG_CONFIG_DIRS=$T/xdg/plasma-mobile:/etc/xdg DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus
XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true KDE_SESSION_VERSION=6 XDG_SESSION_TYPE=wayland"
client_env="$base_env WAYLAND_DISPLAY=s24rt-0 QT_QPA_PLATFORM=wayland GDK_BACKEND=wayland GSK_RENDERER=cairo
KSCREEN_BACKEND_INPROCESS=1 PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell
QT_QUICK_CONTROLS_MOBILE=true QT_QUICK_CONTROLS_STYLE=org.kde.breeze QT_QPA_PLATFORMTHEME=KDE
QT_WAYLAND_DISABLE_WINDOWDECORATION=1"
cl() { n=$1; shift; env -i $client_env setsid "$@" </dev/null >"$T/$n.log" 2>&1 & }
q() { env -i $client_env qdbus6 "$@"; }
kjs() { # kjs NAME FILE: one KWin script, run once in the nested KWin
    q org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$1" >/dev/null 2>&1
    id=$(q org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$2" "$1")
    q org.kde.KWin /Scripting/Script$id org.kde.kwin.Script.run >/dev/null 2>&1
    sleep 0.4
    q org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$1" >/dev/null 2>&1
}
dump() { : >"$T/kwinprobe.log"; kjs s24rt-dump "$T/dump.js"; sleep 0.3; cut -d" " -f2- "$T/kwinprobe.log"; }
shot() { env -i $client_env python3 "$T/shot.py" "$1" "$T/shot-$2.ppm"; }
