// dump.js: the screens, the pointer, and every window's kind, place and screen, to org.s24test.Probe.
function log(t) { callDBus("org.s24test.Probe", "/", "org.s24test.Probe", "Log", t); }
function desc(w) {
    if (!w) return "null";
    const g = w.frameGeometry;
    return [w.resourceClass, "caption=" + JSON.stringify(w.caption), "out=" + (w.output ? w.output.name : "?"),
            Math.round(g.x) + "," + Math.round(g.y) + " " + Math.round(g.width) + "x" + Math.round(g.height),
            w.desktopWindow ? "DESKTOP" : "", w.normalWindow ? "normal" : "", w.dock ? "dock" : "",
            w.noBorder ? "noborder" : "border", w.moveable ? "moveable" : "fixed",
            w.minimized ? "MIN" : "", w.active ? "ACTIVE" : ""].filter(x => x).join(" ");
}
log("screens: " + workspace.screens.map(s => s.name + "@" + s.geometry.x + "," + s.geometry.y + " " + s.geometry.width + "x" + s.geometry.height).join(" | ") + " active=" + (workspace.activeScreen ? workspace.activeScreen.name : "?") + " cursor=" + Math.round(workspace.cursorPos.x) + "," + Math.round(workspace.cursorPos.y));
for (const w of workspace.windowList()) if (w.normalWindow || w.dialog) log("  " + desc(w));
log("  activeWindow: " + desc(workspace.activeWindow));
