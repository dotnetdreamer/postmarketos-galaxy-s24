// place.js: the probe window onto the browser's screen, as a 500x228 window near its middle.
const out = workspace.screens.find(s => s.name === "Virtual-S24Remote");
for (const w of workspace.windowList()) {
    if (w.resourceClass === "org.s24test.probewindow" || w.caption === "s24rt probe") {
        w.setMaximize(false, false);
        w.frameGeometry = { x: out.geometry.x + 400, y: out.geometry.y + 200, width: 800, height: 480 };
    }
}
