// Mouse Mode (Galaxy S24, postmarketOS; Claude, 2026-11-15): the touchscreen as a touchpad, a
// bigger desktop, KWin's zoom following the pointer. /usr/local/bin/s24-mouse-mode does the work;
// its agent owns org.s24.MouseMode on the session bus while mouse mode is on.
import QtQuick 2.15

import org.kde.plasma.private.mobileshell as MobileShell
import org.kde.plasma.private.mobileshell.quicksettingsplugin as QS
import org.kde.plasma.private.mobileshell.state as MobileShellState
import org.kde.plasma.workspace.dbus as DBus

QS.QuickSetting {
    id: root
    text: "Mouse Mode"
    icon: "input-touchpad"
    status: enabled ? "On — swipe down from the top to stop" : "Use the screen as a touchpad"
    enabled: agent.registered

    property bool startRequested: false

    DBus.DBusServiceWatcher {
        id: agent
        busType: DBus.BusType.Session
        watchedService: "org.s24.MouseMode"
    }

    function toggle() {
        if (agent.registered) {
            MobileShell.ShellUtil.executeCommand("/usr/local/bin/s24-mouse-mode off");
        } else {
            // the drawer goes first, so mouse mode starts on the screen underneath
            startRequested = true;
            MobileShellState.ShellDBusClient.closeActionDrawer();
        }
    }

    Connections {
        target: MobileShellState.ShellDBusClient

        function onIsActionDrawerOpenChanged(visible) {
            if (!visible && root.startRequested) {
                root.startRequested = false;
                MobileShell.ShellUtil.executeCommand("/usr/local/bin/s24-mouse-mode on");
            }
        }
    }
}
