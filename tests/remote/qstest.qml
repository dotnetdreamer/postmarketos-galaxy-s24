// qstest.qml: load the Remote Desktop quick setting as the drawer would, and print what it shows.
import QtQuick
Item {
    Loader { id: l; source: "file:///usr/share/plasma/quicksettings/org.s24.quicksetting.remote/contents/ui/main.qml" }
    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: console.log(l.item ? ("text=" + l.item.text + " | status=" + l.item.status + " | enabled=" + l.item.enabled + " | icon=" + l.item.icon + " | settingsCommand=" + l.item.settingsCommand) : ("not loaded: " + l.status))
    }
    Timer { interval: 3500; running: true; onTriggered: Qt.quit() }
}
