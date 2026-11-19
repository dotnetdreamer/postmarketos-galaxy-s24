// qstoggle.qml: tap the Remote Desktop quick setting (its toggle()) twice, printing what it shows.
import QtQuick
Item {
    Loader { id: l; source: "file:///usr/share/plasma/quicksettings/org.s24.quicksetting.remote/contents/ui/main.qml" }
    function show(when) { console.log(when + ": status=" + l.item.status + " | enabled=" + l.item.enabled) }
    Timer { interval: 800; running: true; onTriggered: { show("before"); l.item.toggle() } }
    Timer { interval: 6000; running: true; onTriggered: { show("after the 1st tap"); l.item.toggle() } }
    Timer { interval: 10000; running: true; onTriggered: { show("after the 2nd tap"); Qt.quit() } }
}
