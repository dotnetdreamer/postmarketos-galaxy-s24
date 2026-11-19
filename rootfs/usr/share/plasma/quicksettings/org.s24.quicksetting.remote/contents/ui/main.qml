// Remote Desktop (Galaxy S24, postmarketOS; Claude, 2026-11-19): this phone's desktop in any
// browser, from anywhere. /usr/local/bin/s24-remote does the work; while it is on it owns
// org.s24.Remote on the session bus, and its Summary property is the line under the name: the code
// to type, or that a browser is connected. A long press shows the notification with the address.
import QtQuick 2.15

import org.kde.plasma.private.mobileshell as MobileShell
import org.kde.plasma.private.mobileshell.quicksettingsplugin as QS
import org.kde.plasma.workspace.dbus as DBus

QS.QuickSetting {
    id: root
    text: "Remote Desktop"
    icon: "krfb"
    enabled: daemon.registered
    status: {
        if (!daemon.registered) {
            return "Use this desktop from a browser";
        }
        const summary = props.item ? props.item.properties.Summary : undefined;
        return summary ? summary : "Starting…";
    }
    settingsCommand: "/usr/local/bin/s24-remote show"

    DBus.DBusServiceWatcher {
        id: daemon
        busType: DBus.BusType.Session
        watchedService: "org.s24.Remote"
    }

    // Made again each time Remote Desktop comes on, so it reads the new copy's properties.
    Loader {
        id: props
        active: daemon.registered
        sourceComponent: DBus.Properties {
            busType: DBus.BusType.Session
            service: "org.s24.Remote"
            path: "/org/s24/Remote"
            iface: "org.s24.Remote"
        }
    }

    function toggle() {
        MobileShell.ShellUtil.executeCommand(daemon.registered ? "/usr/local/bin/s24-remote off" : "/usr/local/bin/s24-remote on");
    }
}
