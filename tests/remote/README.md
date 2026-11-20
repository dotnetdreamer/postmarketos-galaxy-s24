# Remote Desktop tests

How [Remote Desktop](../../docs/remote-desktop.md) was tested, and the scripts to do it again. Copy this folder to `/tmp/s24rt` on the phone; `drive.mjs` runs on a PC with Node and Edge or Chrome.

Unit tests first: `cargo test` in `src/remote` (on the phone, or built on a PC for `aarch64-unknown-linux-musl` and copied over: they need Linux). They cover the code and its limits, the pace and the bit rate (`flow.rs`), H.264 to the AVC form, the settings file, the tunnel's address in cloudflared's log.

## A desktop nobody is using: the nested session

Typing, clicking and dragging must never go into a phone someone may be holding, or one that is locked. So the desktop itself was tested in a **private nested session** on the phone: its own D-Bus, its own KWin (`kwin_wayland --virtual`, a phone-sized screen, no input devices), Plasma Mobile's real shell, private config folders. Only `XDG_RUNTIME_DIR` is shared, for PipeWire.

| Script | |
|---|---|
| `t.sh` | Sourced: the session's environment, `cl NAME cmd` (a client of it, logging to `NAME.log`), `q` (qdbus6 on its bus), `kjs` (run a KWin script once), `dump` (screens, pointer, windows), `shot SCREEN NAME` |
| `setup.sh` | Bus, KWin, Plasma Mobile, a log receiver for KWin scripts, and `s24-remote serve` on port 7181 with a 1-minute grace (`S24RT_TUNNEL=none` for no tunnel) |
| `remote.sh` | Starts `s24-remote` again (`EXTRA='VAR=value'` adds to its environment, which the screen program inherits); prints the address and code |
| `teardown.sh` | Stops everything on the test bus, found by environment, never by a `pgrep -f` pattern (those match your own SSH command and kill it) |
| `probe.py` | A GTK 4 window that writes down every key, click, scroll and the text typed into it |
| `anim.py`, `anim.sh`, `place.js` | A window that changes every frame, put on the browser's screen by a KWin script |
| `probe.sh` | Opens the probe again |
| `dump.js`, `receiver.py`, `bus.conf` | KWin script that reports windows over D-Bus, its receiver, the private bus |
| `shot.py`, `shrink.py`, `crop.py` | KWin's picture of a screen, made smaller or cut out for copying off the phone |
| `cpu.sh SECONDS` | CPU of the screen program, `s24-remote` and the test KWin |
| `drawer.sh off\|on`, `qstest.qml`, `qstoggle.qml` | The tile: the drawer opened over D-Bus, and its QML loaded with `qmlscene` against the test bus (what it shows, and `toggle()` turning Remote Desktop on and off) |

The browser: `node drive.mjs <edge or chrome> <work dir> 9471 1600x900` keeps a headless browser open and takes commands on `http://127.0.0.1:9471`: `/open?url=`, `/code?c=`, `/state`, `/click?screen=1&x=&y=&b=right`, `/move`, `/drag?screen=1&x1&y1&x2&y2`, `/type?text=`, `/key?key=Enter&code=Enter&vk=13`, `/wheel?screen=1&x&y&dy=`, `/size?w=&h=`, `/js?e=`, `/shot?name=`. `screen=1` takes coordinates on the phone's screen. Input goes in as CDP input events, so it passes through the page's own handlers like a real mouse and keyboard. Remember `curl -g` (or `%5B`, `%5D`) for selectors with brackets in `/js`.

What was checked this way (2026-11-19): a wrong code refused, the right one in; typing (capitals, punctuation, Enter, `€`), click, right click, wheel, dragging a window by its title bar, a window opened on the browser's screen staying there, docked mode on and back off, Low/Medium/High, resizing the browser, a second browser taking over and the first taking back, the browser silent for 6 s (blocked its own acknowledgements in the page), turning off. The numbers are in the doc.

Traps:

- An app started from SSH more than 1.5 s after the browser's last click opens on the phone's screen, not the browser's: the screen program's KWin script (`here.js`) puts windows where they were opened from, and with no recent click the phone is that place. Click, then start the app at once, or place it with a KWin script
- Docked mode's KWin rules go to `$XDG_CONFIG_HOME/plasma-mobile/kwinrc`, which KWin reads through `XDG_CONFIG_DIRS`: in the nested session that is `$T/xdg/plasma-mobile`
- `curl` reads `[…]` in a URL as a pattern unless given `-g`

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

The lock screen (a nested KWin with its screen locker and Plasma Mobile's lock screen, its log kept):

| Script | |
|---|---|
| `lockrepro.sh lock\|screen\|panel\|log` | Start it and lock it; a browser's screen made and put away; the phone-sized screen off and on; the lock screen's crashes in KWin's log |
| `holdscreen.sh start\|stop` | A browser's screen that stays up (the helper fed heartbeats, no browser) |
| `notetest.sh` | Remote Desktop on and off with its notifications, five times |
| `reshell-nested.sh` | `plasmashell --replace` in the nested session |

Checked on the real phone, locked (2026-11-20): the tunnel and sign-in, the lock screen in the browser with its note, the sleep lock held, the phone's panel off and its touchscreen held, the grace period running out (locked, panel back but dark, touch let go, docked mode back off), the power key both ways (pressed by `powerkey.py`, not by hand), the browser's screen dark on an idle lock screen and woken by a pointer move from the browser (the phone's panel stayed dark), turning off. The panel switched off by KWin itself when the browser's screen came back (KWin's memory of that pair of screens) was found here, and handled. Over mobile data alone (Wi-Fi switched off for 150 s): the address kept working (cloudflared reconnected by itself), sign-in, the picture (the lock screen).

Not done on the real phone: a finger on the tile in the drawer (the tile's QML was checked with `qmlscene`, and Remote Desktop was started with `realstart-root.sh`) and a real press of the power button. The install script was run on the phone with `S24_REMOTE_BIN`: a binary from the same `cargo build --release --locked`, run by hand (70 s), so the script's own cargo step hasn't run inside it.

The lock screen broke during these tests: by 00:33 kscreenlocker was in its emergency mode (four crashes of the lock screen process since the last unlock). See [remote-desktop.md](../../docs/remote-desktop.md), "Not done". On a fresh locked nested session, none of what was tried crashed the lock screen: notifications, browser screens added and removed, the panel off and on (no browser signed in; the nested session has no touchscreen or power key). The only nested crash followed a plasmashell restart.
