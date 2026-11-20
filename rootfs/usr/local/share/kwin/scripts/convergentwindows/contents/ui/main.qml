// SPDX-FileCopyrightText: 2023 Plata Hill <plata.hill@kdemail.net>
// SPDX-FileCopyrightText: 2023 Devin Lin <devin@kde.org>
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Galaxy S24 (postmarketos-galaxy-s24): Plasma Mobile's convergentwindows script, decided per screen.
//
// Plasma Mobile's docked mode is one switch for the whole phone: with it on, every screen gets desktop
// windows, the phone's own panel too. Here, in docked mode with another screen beside the phone's
// panel (Remote Desktop's browser screen, a monitor), the phone's panel stays a phone, as with Samsung
// DeX: its windows fill it without title bars, while the other screens get windows with title bars
// that move and resize. A window that goes from the phone to a desktop screen gets a window's size
// there. Phone mode, and docked mode on the phone alone, work as Plasma Mobile's own script does.
//
// Docked mode is read from KWin's own window rules, not from KWin's copy of Plasma Mobile's setting:
// whatever switches the mode (Plasma Mobile's quick setting, s24-dockd, Open Android DeX's screen
// program) runs plasma-mobile-envmanager, which gives KWin centred placement for docked mode and
// maximizing for a phone and has KWin reload them. KWin's copy of the setting has been seen to miss a
// change, leaving windows the wrong way.
//
// Installed in /usr/local/share/kwin/scripts, which KWin searches before /usr/share: it stands in for
// Plasma Mobile's script of the same name. For tests, kwinrc [Script-convergentwindows] phoneScreen
// names the screen to treat as the phone's panel.

import QtQuick
import org.kde.kwin as KWinComponents
import org.kde.plasma.private.mobileshell.shellsettingsplugin as ShellSettings

Item {
    id: root

    // Windows this script maximized for a phone screen (by internalId): they get a window's size back
    // when they move to a desktop screen. And the windows already connected to.
    property var fitted: ({})
    property var watched: ({})

    readonly property string testScreen: (typeof KWin !== "undefined" && KWin.readConfig) ? String(KWin.readConfig("phoneScreen", "")) : ""

    function isPhonePanel(output) {
        if (!output) {
            return false;
        }
        const name = output.name;
        if (testScreen !== "") {
            return name === testScreen;
        }
        return name.startsWith("DSI") || name.startsWith("eDP") || name.startsWith("LVDS");
    }

    // KWin::Options::PlacementMaximizing: Plasma Mobile's phone mode (docked mode is Centered).
    readonly property int maximizing: 9

    function docked() {
        const placement = KWinComponents.Options.placement;
        if (placement === undefined) {
            return ShellSettings.Settings.convergenceModeEnabled;
        }
        return placement !== maximizing;
    }

    // Phone-like: filling its screen without title bars.
    function phoneLike(window) {
        if (!docked()) {
            return true;
        }
        return isPhonePanel(window.output) && KWinComponents.Workspace.screens.length > 1;
    }

    function run(window) {
        if (!window) {
            return;
        }
        // HACK: don't maximize xwaylandvideobridge
        // see: https://invent.kde.org/plasma/plasma-mobile/-/issues/324
        if (window.resourceClass === 'xwaylandvideobridge') {
            return;
        }

        if (!window.normalWindow) {
            return;
        }

        const key = String(window.internalId);
        if (!phoneLike(window)) {
            window.noBorder = false;
            // Fitted to the phone, now on a desktop screen: a window's size, in the middle.
            if (fitted[key] && !isPhonePanel(window.output)) {
                delete fitted[key];
                const desktop = window.desktops[0];
                if (window.maximizable && !window.fullScreen && desktop !== undefined) {
                    window.setMaximize(false, false);
                    const area = KWinComponents.Workspace.clientArea(KWinComponents.Workspace.PlacementArea, window.output, desktop);
                    const w = Math.round(area.width * 0.7);
                    const h = Math.round(area.height * 0.75);
                    window.frameGeometry = Qt.rect(area.x + Math.round((area.width - w) / 2), area.y + Math.round((area.height - h) / 2), w, h);
                }
            }
            return;
        }

        if (!window.fullScreen) {
            const output = window.output;
            const desktop = window.desktops[0]; // assume it's the first desktop that the window is on
            if (desktop === undefined) {
                return;
            }
            const maximizeRect = KWinComponents.Workspace.clientArea(KWinComponents.Workspace.MaximizeArea, output, desktop);

            // set the window to the maximized size and position instantly, avoiding race condition
            // between maximizing and window decorations being turned off (changing window height)
            // see: https://invent.kde.org/teams/plasma-mobile/issues/-/issues/256
            window.frameGeometry = maximizeRect;
        }

        // turn off window decorations
        window.noBorder = true;

        if (!window.fullScreen) {
            // run maximize after to ensure the state is maximized
            window.setMaximize(true, true);
            fitted[key] = true;
        }
    }

    function watch(window) {
        const key = String(window.internalId);
        if (watched[key]) {
            return;
        }
        watched[key] = true;
        // These outlive this script if KWin unloads it (root is null then): they check.
        const again = () => {
            if (root) {
                root.run(window);
            }
        };
        window.interactiveMoveResizeFinished.connect(again);
        window.outputChanged.connect(() => {
            if (root) {
                root.run(window);
                settle.restart();
            }
        });
        window.fullScreenChanged.connect(again);
        window.maximizedChanged.connect(() => {
            if (root && window.maximizable) {
                root.run(window);
            }
        });
        window.closed.connect(() => {
            if (root) {
                delete root.watched[key];
                delete root.fitted[key];
            }
        });
    }

    function runAll() {
        const windows = KWinComponents.Workspace.windows;
        for (let i = 0; i < windows.length; i++) {
            if (windows[i].normalWindow) {
                root.run(windows[i]);
            }
        }
    }

    // KWin finishes moving windows off a screen that went (and puts back sizes it remembers) after
    // the signals this script hears: everything once more when that has settled.
    Timer {
        id: settle
        interval: 500
        onTriggered: root.runAll()
    }

    Component.onCompleted: {
        // Loaded into a running KWin (the install does that): the windows already there.
        const windows = KWinComponents.Workspace.windows;
        for (let i = 0; i < windows.length; i++) {
            if (windows[i].normalWindow) {
                root.watch(windows[i]);
            }
        }
        root.runAll();
    }

    Connections {
        target: ShellSettings.Settings

        function onConvergenceModeEnabledChanged() {
            root.runAll();
            settle.restart();
        }
    }

    Connections {
        target: KWinComponents.Options

        function onPlacementChanged() {
            root.runAll();
            settle.restart();
        }

        function onConfigChanged() {
            settle.restart();
        }
    }

    Connections {
        target: KWinComponents.Workspace

        function onWindowAdded(window) {
            if (window.normalWindow) {
                root.watch(window);
                root.run(window);
            }
        }

        function onWindowActivated(window) {
            if (window && window.normalWindow) {
                root.watch(window);
                root.run(window);
            }
        }

        function onScreensChanged() {
            // Windows are moved from the external screen
            // to the internal screen if the external screen
            // is disconnected.
            root.runAll();
            settle.restart();
        }
    }
}
