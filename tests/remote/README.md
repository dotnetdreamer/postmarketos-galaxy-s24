# Remote Desktop tests

How [Remote Desktop](../../docs/remote-desktop.md) was tested, and the scripts to do it again. Copy this folder to `/tmp/s24rt` on the phone; `drive.mjs` runs on a PC with Node and Edge or Chrome.

Unit tests first: `cargo test` in `src/remote` (on the phone, or built on a PC for `aarch64-unknown-linux-musl` and copied over: they need Linux). They cover the code and its limits, the pace and the bit rate (`flow.rs`), H.264 to the AVC form, the settings file, the tunnel's address in cloudflared's log, and WebRTC: the STUN answer, which addresses are offered, a whole session against str0m playing the browser over real UDP on 127.0.0.1.

## A desktop nobody is using: the nested session

Typing, clicking and dragging must never go into a phone someone may be holding, or one that is locked. So the desktop itself was tested in a **private nested session** on the phone: its own D-Bus, its own KWin (`kwin_wayland --virtual`, a phone-sized screen, no input devices), Plasma Mobile's real shell, private config folders. Only `XDG_RUNTIME_DIR` is shared, for PipeWire. The test's `s24-remote` keeps its pid and status in a folder of its own (`/run/user/10000/s24rt-run`, with PipeWire's and the test KWin's sockets linked in), so the real Remote Desktop can stay on meanwhile; `BIN=` runs a build that isn't installed. Its settings say `phone_screen = always`: a test browser can then never switch the phone's own screen off and hold its touchscreen, which is the real one even for a test session.

| Script | |
|---|---|
| `t.sh` | Sourced: the session's environment, `cl NAME cmd` (a client of it, logging to `NAME.log`), `q` (qdbus6 on its bus), `kjs` (run a KWin script once), `dump` (screens, pointer, windows), `shot SCREEN NAME` |
| `setup.sh` | Bus, KWin, Plasma Mobile, a log receiver for KWin scripts, and `s24-remote serve` on port 7181 with a 1-minute grace (`S24RT_TUNNEL=none` for no tunnel) |
| `remote.sh` | Starts `s24-remote` again (`EXTRA='VAR=value'` adds to its environment, which the screen program inherits); prints the address and code |
| `teardown.sh` | Stops everything on the test bus, found by environment, never by a `pgrep -f` pattern (those match your own SSH command and kill it) |
| `probe.py` | A GTK 4 window that writes down every key, click, scroll and the text typed into it |
| `anim.py`, `anim.sh`, `place.js` | A window that changes every frame, put on the browser's screen by a KWin script |
| `probe.sh` | Opens the probe again |
| `flip.py`, `flip.sh`, `place-flip.js` | A window that turns black or white at each key or click and changes nothing else, put on the browser's screen and focused |
| `latency.js` | Run in the page (`/js`): `s24Latency(n, x, y, mode)` presses a key the way the page sends one, over whichever route it uses, and times the picture at (x, y) changing. `raf` (the default) looks when the page draws, the same for both routes; `tight` looks every millisecond but sees the WebSocket's canvas before it is shown, so it favours the WebSocket |
| `dump.js`, `receiver.py`, `bus.conf` | KWin script that reports windows over D-Bus, its receiver, the private bus |
| `shot.py`, `shrink.py`, `crop.py` | KWin's picture of a screen, made smaller or cut out for copying off the phone |
| `cpu.sh SECONDS` | CPU of the screen program, `s24-remote` and the test KWin |
| `drawer.sh off\|on`, `qstest.qml`, `qstoggle.qml` | The tile: the drawer opened over D-Bus, and its QML loaded with `qmlscene` against the test bus (what it shows, and `toggle()` turning Remote Desktop on and off) |

The browser: `node drive.mjs <edge or chrome> <work dir> 9471 1600x900 [visible]` keeps a browser open (headless, or a real window with `visible`) and takes commands on `http://127.0.0.1:9471`: `/open?url=`, `/code?c=`, `/state`, `/click?screen=1&x=&y=&b=right`, `/move`, `/drag?screen=1&x1&y1&x2&y2`, `/type?text=`, `/key?key=Enter&code=Enter&vk=13`, `/wheel?screen=1&x&y&dy=`, `/size?w=&h=`, `/js?e=`, `/shot?name=`, `/proceed` (past the "not private" warning, Advanced then Continue, as a person clicks them), `/init?js=` (a script run before every page's own). `screen=1` takes coordinates on the phone's screen. Input goes in as CDP input events, so it passes through the page's own handlers like a real mouse and keyboard. Remember `curl -g` (or `%5B`, `%5D`) for selectors with brackets in `/js`. A visible window that isn't in front gets no typed text (`/code` types): set the field and send it an `input` event through `/js` instead. The visible window starts with Edge's occlusion detection off: otherwise a window covered by someone's work counts as hidden, and a hidden page shows no WebRTC video.

WebRTC was checked this way (2026-11-20, version 1.2.0): connecting through the Wi-Fi door and through the tunnel, the picture moving to WebRTC and back with the menu's button, a key to the picture timed on both routes (`flip.py`, `latency.js`; the numbers are in the doc), typing, a click and a scroll over the data channel into `probe.py`, a window animating on High on both routes, no way through (the page's `RTCPeerConnection` forced to relays only: the WebSocket kept the picture through three tries), and the page hidden for 6 s (the test window minimised without being activated) staying on WebRTC. Then on the real session with the build installed, the test browser only watching: WebRTC through the Wi-Fi address and through the tunnel, host to host, nothing lost.

What was checked this way (2026-11-19): a wrong code refused, the right one in; typing (capitals, punctuation, Enter, `€`), click, right click, wheel, dragging a window by its title bar, a window opened on the browser's screen staying there, docked mode on and back off, Low/Medium/High, resizing the browser, a second browser taking over and the first taking back, the browser silent for 6 s (blocked its own acknowledgements in the page), turning off. The numbers are in the doc.

Traps:

- An app started from SSH more than 1.5 s after the browser's last click opens on the phone's screen, not the browser's: the screen program's KWin script (`here.js`) puts windows where they were opened from, and with no recent click the phone is that place. Click, then start the app at once, or place it with a KWin script
- Docked mode's KWin rules go to `$XDG_CONFIG_HOME/plasma-mobile/kwinrc`, which KWin reads through `XDG_CONFIG_DIRS`, as `startplasmamobile` sets it: in the nested session `$T/config/plasma-mobile`, so that the envmanager's rules reach the nested KWin as they reach the real one
- `curl` reads `[…]` in a URL as a pattern unless given `-g`

## The phone as a phone beside the browser's screen

The window script per screen (`rootfs/usr/local/share/kwin/scripts/convergentwindows`) and the second screen's panels, in the nested session. No `s24-remote` runs there: a second one would share the real one's files in `$XDG_RUNTIME_DIR`.

| Script | |
|---|---|
| `dexlab.sh start\|hold\|unhold\|panels\|addpanels\|prio\|dump\|script\|stop` | The nested session with a phone-sized screen (`Virtual-0` stands in for DSI-1: kwinrc `[Script-convergentwindows] phoneScreen`), the browser's screen held up, the panels per screen, the window script swapped in |
| `dexseq.sh [ROUNDS]` | Every move, in a fresh nested session: a window on the phone beside the browser's screen, the phone's screen off and on, sent to either screen, the browser's screen gone, docked mode by hand on the phone alone |
| `panels2.js` | The second screen's panels (the same script `s24-remote` runs, `src/remote/src/panels.rs`) |
| `lockonstart.sh with\|without` | A nested KWin with its screen locker, started with or without the automatic login's config folder: locked or not, with nothing asking it to lock |

Checked (2026-11-20): `dexseq.sh 2`, every move in both rounds as it should be, no script errors; `lockonstart.sh`, locked with the folder (twice) and not without it (twice); `panels2.js`, a status bar and a navigation bar made on the second screen, once, and both screens with their own whichever is first (also with the phone's screen switched off and on).

Traps:

- The nested KWin died every time a test script called `workspace.sendClientToScreen` (Plasma Mobile's own window script loaded or not): move windows by their `frameGeometry`
- KWin keeps a QML script compiled by its file name: a script changed and loaded again under the same path runs the old one. Start a new KWin to try a new version
- Unloading a QML script leaves its window signal connections behind, now calling into a script that is gone (`TypeError: Cannot read property 'run' of null`): the script checks for that
- KWin's copy of Plasma Mobile's docked-mode setting (`ShellSettings.Settings.convergenceModeEnabled`) missed changes written with `kwriteconfig6 --notify`; KWin's own window rules (`Options.placement`), which the envmanager sets and reloads, didn't
- With phone mode's rules from the start (TabletMode=auto) the nested KWin, which has no touchscreen, has died as the screen program connected: `dexseq.sh` turns docked mode on first
- A stale `s24rt-0` socket from a killed KWin passes "wait for the socket": remove it before starting one

## The real session, without touching it

The real session was tested only in ways that need no input on a locked phone:

| Script | |
|---|---|
| `real.sh` | Sourced: the real session's addresses, from its plasmashell (the one in the seat's session cgroup, never a test copy) |
| `realcheck.sh` | Read only: locked or not, the screens, sleep locks, `s24-remote status` |
| `realstart-root.sh on\|off` | As root: joins the desktop session's cgroup, then as the user runs `s24-remote`. That is where the tile starts it from, and logind lets only the session's own programs block sleep (from SSH: `AccessDenied`) |
| `grabcheck.py` | Whether the touchscreen is held (a second grab gets `EBUSY`) |
| `powerkey.py` | As root: one press of the power key, written into its own input device the way `s24-powerd` injects it |
| `reshell.py`, `reshell-root.sh` | Plasma's shell started again inside the session, with its exact environment. **Don't, while the phone is locked**: see below |
| `mobiletest-root.sh`, `wifi60-root.sh` | As root: Wi-Fi off for 150 or 60 s, then on again by itself, so a browser can be tried on mobile data alone |
| `restart-root.sh` | As root: Remote Desktop off and on in the session (a newly installed `s24-remote`), its status once the address works |
| `sessionend-root.sh` | As root: Remote Desktop ended as a logout ends it (SIGTERM), then `s24-remote login` in the session, as the login autostart runs it |
| `panels.sh` | Read only: Plasma's panels and desktops per screen in the real session |

The lock screen (a nested KWin with its screen locker and Plasma Mobile's lock screen, its log kept):

| Script | |
|---|---|
| `lockrepro.sh lock\|screen\|panel\|log` | Start it and lock it; a browser's screen made and put away; the phone-sized screen off and on; the lock screen's crashes in KWin's log |
| `holdscreen.sh start\|stop` | A browser's screen that stays up (the helper fed heartbeats, no browser) |
| `notetest.sh` | Remote Desktop on and off with its notifications, five times |
| `reshell-nested.sh` | `plasmashell --replace` in the nested session |

Checked on the real phone, locked (2026-11-20): the tunnel and sign-in, the lock screen in the browser with its note, the sleep lock held, the phone's panel off and its touchscreen held, the grace period running out (locked, panel back but dark, touch let go, docked mode back off), the power key both ways (pressed by `powerkey.py`, not by hand), the browser's screen dark on an idle lock screen and woken by a pointer move from the browser (the phone's panel stayed dark), turning off. The panel switched off by KWin itself when the browser's screen came back (KWin's memory of that pair of screens) was found here, and handled. Over mobile data alone (Wi-Fi switched off for 150 s): the address kept working (cloudflared reconnected by itself), sign-in, the picture (the lock screen).

Not done on the real phone: a finger on the tile in the drawer (the tile's QML was checked with `qmlscene`, and Remote Desktop was started with `realstart-root.sh`) and a real press of the power button. The install script was run on the phone with `S24_REMOTE_BIN`: a binary from the same `cargo build --release --locked`, run by hand (70 s), so the script's own cargo step hasn't run inside it.

Checked on the real phone (2026-11-20, version 1.1.0; the phone locked, then unlocked, not by these tests), with a visible Edge on a PC on the same Wi-Fi: the Wi-Fi address past the certificate warning, a secure page with WebCodecs, sign-in with the code, the picture; the password set from Access (a short one and two that differ refused), a wrong one refused at sign-in, the right one in on the Wi-Fi address and through the tunnel, removed; the second screen's panels made (log) and on both screens (`panels.sh`); the stay-on flag through a SIGTERM, and `s24-remote login` starting Remote Desktop again. The test browser asked for the phone's screen to stay on (an `/init` script set `phoneScreen` in the page's hello), so that the owner's phone wasn't switched off under them.

A restart, by the owner (12:14): the automatic login, Remote Desktop on again by itself, the session's environment as it should be; locked at the start as far as `s24-remote login` could see (it found it locked). Not run: a finger on the phone's screen beside the browser's.

The lock screen broke during these tests: by 00:33 kscreenlocker was in its emergency mode (four crashes of the lock screen process since the last unlock). See [remote-desktop.md](../../docs/remote-desktop.md), "Not done". On a fresh locked nested session, none of what was tried crashed the lock screen: notifications, browser screens added and removed, the panel off and on (no browser signed in; the nested session has no touchscreen or power key). The only nested crash followed a plasmashell restart.
