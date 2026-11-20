//! Plasma Mobile's two panels (the status bar on top, with the drawer it pulls down; the navigation
//! bar or gesture area below) for the second screen too.
//!
//! Plasma puts a session's panels on its first screen. With the browser's screen beside the phone's
//! panel, KWin may make the browser's screen the first (it does after the panel was switched off and
//! on again), and then the phone's panel had no status bar to pull down and no gestures: not a phone
//! any more. So a second pair is made once, for screen 1: with two screens each has its own, whichever
//! is first; with one screen, the second pair isn't shown. A monitor beside the phone gets them too.
//!
//! Made through plasmashell's own scripting (evaluateScript), marked in their configuration
//! (S24Remote/secondScreen) so that they are made only once. tests/remote/panels2.js is the same
//! script, for the nested test session.

use crate::say;

const SCRIPT: &str = r#"
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
    return "made " + type + " (" + location + ")";
}
var out = [];
if (!have.top) out.push(add("org.kde.plasma.mobile.panel", "top"));
if (!have.bottom) out.push(add("org.kde.plasma.mobile.taskpanel", "bottom"));
print(out.length ? out.join(", ") : "there already");
"#;

/// The second screen's panels, made if they aren't there. Only in Plasma Mobile.
pub async fn ensure(bus: &zbus::Connection) {
    let mobile = std::env::var("PLASMA_PLATFORM").map(|p| p.starts_with("phone")).unwrap_or(false);
    if !mobile {
        return;
    }
    let r = bus
        .call_method(Some("org.kde.plasmashell"), "/PlasmaShell", Some("org.kde.PlasmaShell"), "evaluateScript", &(SCRIPT,))
        .await;
    match r {
        Ok(m) => {
            let out = m.body().deserialize::<String>().unwrap_or_default();
            say!("panels for the second screen: {}", out.trim());
        }
        Err(e) => say!("panels for the second screen: {e}"),
    }
}
