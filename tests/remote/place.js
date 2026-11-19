// place.js: the test animation onto the browser's screen, as a 500x228 window near its middle.
const out = workspace.screens.find(s => s.name === "Virtual-S24Remote");
for (const w of workspace.windowList()) {
    if (w.resourceClass === "org.s24test.anim" || w.caption === "s24rt anim") {
        w.setMaximize(false, false);
        w.frameGeometry = { x: out.geometry.x + 550, y: out.geometry.y + 336, width: 500, height: 228 };
    }
}
