// panels2.js: Plasma Mobile's two panels (status bar, navigation) for the second screen too, once.
// Plasma puts its panels on the first screen; this pair is on screen 1, so with two screens (the
// phone's panel beside Remote Desktop's browser screen, or a monitor) each has its own. Marked in
// their config (S24Remote/secondScreen) so that they are made only once.
var have = { top: false, bottom: false };
var ps = panels();
for (var i = 0; i < ps.length; i++) {
    ps[i].currentConfigGroup = ["S24Remote"];
    var mark = ps[i].readConfig("secondScreen", false);
    if (mark === true || mark === "true") {
        have[ps[i].location] = true;
    }
}
function add(type, location) {
    var p = new Panel(type);
    p.location = location;
    p.currentConfigGroup = ["S24Remote"];
    p.writeConfig("secondScreen", true);
    p.screen = 1;
    return "added " + type + " (" + location + "), now on screen " + p.screen;
}
var out = [];
if (!have.top) out.push(add("org.kde.plasma.mobile.panel", "top"));
if (!have.bottom) out.push(add("org.kde.plasma.mobile.taskpanel", "bottom"));
print(out.length ? out.join("\n") : "the second screen has its panels already");
