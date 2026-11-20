#!/bin/sh
# panels.sh: read only. Which Plasma containment (panels, desktops) sits on which screen in the real session.
. /tmp/s24rt/real.sh
qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript '
var s = "screens: " + screenCount + "\n";
for (var i = 0; i < screenCount; i++) { var g = screenGeometry(i); s += "  screen " + i + ": " + g.x + "," + g.y + " " + g.width + "x" + g.height + "\n"; }
var p = panels();
for (var i = 0; i < p.length; i++) s += "panel " + p[i].type + " id " + p[i].id + " screen " + p[i].screen + " location " + p[i].location + "\n";
var d = desktops();
for (var i = 0; i < d.length; i++) s += "desktop " + d[i].type + " id " + d[i].id + " screen " + d[i].screen + "\n";
print(s);' 2>&1
