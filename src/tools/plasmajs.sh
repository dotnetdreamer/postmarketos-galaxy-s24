#!/bin/sh
# Run a Plasma shell script in the user's session and print its output (Claude, 2026-11-15)
P=$(pgrep -u user -x plasmashell | head -1)
E=/tmp/claude-session.env
tr '\0' '\n' < /proc/$P/environ | grep -E '^(DBUS_SESSION_BUS_ADDRESS|WAYLAND_DISPLAY|XDG_RUNTIME_DIR)=' | sed 's/^/export "/; s/$/"/' > $E
chmod 644 $E; chmod 644 "$1"
su user -s /bin/sh -c ". $E; qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript \"\$(cat $1)\""
rm -f $E
