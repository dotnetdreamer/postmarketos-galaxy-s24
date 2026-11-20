// place-flip.js: the flip window onto the browser's screen at (100, 100), 400x300.
const out = workspace.screens.find(s => s.name === "Virtual-S24Remote");
for (const w of workspace.windowList()) {
    if (w.resourceClass === "org.s24test.flip" || w.caption === "s24rt flip") {
        w.setMaximize(false, false);
        w.frameGeometry = { x: out.geometry.x + 100, y: out.geometry.y + 100, width: 400, height: 300 };
        workspace.activeWindow = w;
    }
}
