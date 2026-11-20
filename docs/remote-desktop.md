# Remote Desktop: the phone's desktop in any browser

**State:** works. Turn on **Remote Desktop** in the drawer, open the address it shows in any browser on any computer, wherever the phone and the computer have internet (tested with the phone on home Wi-Fi and on mobile data alone), type the code, and the phone's desktop is there: a screen of its own in docked mode, with windows you can move, your mouse and your keyboard. Nothing to install on the computer, no account, no VPN app. **Low**, **Medium** and **High** quality are at the top of the page; Medium is the default.

Also:

- **On the same Wi-Fi**, a second address, `https://<the phone's address>:7443`, goes straight to the phone, not through the internet
- **A password of your own**, set from the page, signs in from any browser without the code
- **It stays on**: once turned on, it is on again after a restart, until it is turned off (the phone starts its desktop by itself, locked)
- **The phone stays a phone** next to the browser's desktop, as with Samsung DeX: its own status bar and gestures, its apps full screen

## Using it

1. Pull down the drawer and tap **Remote Desktop**. In a few seconds a notification shows the address (`https://<four words>.trycloudflare.com`), the address on this Wi-Fi, and an 8-digit code. The tile shows the code too. Long-press the tile to see the notification again; its **Copy link** puts the address, code included, on the phone's clipboard. It stays on, through restarts too, until you tap the tile again
2. On the computer, open the address and type the code. The code works once: the phone shows a new one for the next browser. With a password of your own (below), **Use your password instead** takes that; the browser remembers which you use
3. If the phone is locked, the browser shows the lock screen with a note: type the phone's PIN there
4. Move the mouse to the top middle of the page (or tap the little bar there) for the menu:
   - **Low / Medium / High**: see below
   - **Full screen**: in Chrome and Edge, keys like Esc and Alt+Tab then go to the phone
   - **Keyboard**: brings up the on-screen keyboard on a tablet or phone
   - **Clipboard**: copy on one side, paste on the other (the browser asks once for permission)
   - **Phone screen**: whether the phone's own screen stays on while you are connected (off by default)
   - **Info**: frames a second, speed and delay
   - **Access**: the phone's two addresses, and your own password (set, change, remove)
   - **Disconnect**
5. To stop, tap the tile again, or **Turn off** in the notification

Quality:

| | Screen (long side, at most) | Frames a second (at most) | Bit rate (starts at, at most) |
|---|---|---|---|
| Low | 1280 px | 20 | 0.8, 1.2 Mbit/s |
| Medium | 1920 px | 30 | 2.5, 4 Mbit/s |
| High | 2560 px | 60 | 6, 12 Mbit/s |

The phone's screen for the browser is the size of the browser window (up to the long side above), so the picture is shown pixel for pixel. On Low a big window gets a smaller screen, scaled up in the browser. The bit rate follows the connection in any case: it goes down when the link can't keep up and climbs back when it can.

While a browser is connected:

- The phone's own screen is off and its touchscreen ignores touches, so the phone can stay in a pocket. **The power button gives the phone back**: one press locks it and turns its screen on; another press turns the phone's screen off again, and the browser's stays on. **Phone screen** in the page's menu keeps it on from the start
- With its screen on, the phone stays a phone: its status bar to pull down, its navigation gestures, its apps full screen and without title bars. The browser's screen is the desktop beside it, with its own status bar and navigation bar, and windows with title bars. An app moved from one to the other changes with it
- The phone doesn't sleep while Remote Desktop is on (a sleeping phone is off the internet, and nothing a browser sends wakes it). That costs battery: turn it off when you don't need it
- When the browser goes (closed, or the connection drops), the desktop waits 5 minutes for it with the windows where they were. After that the phone is locked, and its screen comes back but stays dark

## On the phone's Wi-Fi

A computer on the same Wi-Fi as the phone (or on its USB cable's network) can use `https://<the phone's address>:7443`, shown in the notification, by `s24-remote status`, and under **Access** in the page. It goes straight to the phone: no internet, no Cloudflare, and a quicker picture (a round trip of about 20 ms at home against about 50 ms through Cloudflare).

The first time, the browser warns that the connection isn't private: nobody can vouch for a phone's own address, so the phone made its own certificate. Choose **Advanced**, then **Continue** (Chrome, Edge, Brave), or **Show Details**, **visit this website** (Safari). From then on it is a secure page like any other, and the sign-in is the same as on the internet address. The certificate stays the same while the phone's address does.

The same code or password signs in. `lan = off` in the settings turns this address off.

## Your own password

Under **Access** in the page (once signed in with the code), set a password of 8 characters or more. From then on, **Use your password instead** on the sign-in page takes it, on both addresses, from any browser: no need to look at the phone for a code. The code keeps working too. The phone shows a notification whenever the password is set or removed.

At the phone, `s24-remote password` sets it and `s24-remote password --remove` removes it.

## On through a restart

Once it is turned on, Remote Desktop stays on: after the phone restarts it is on again, until it is turned off with the tile or **Turn off** in the notification.

A phone that restarts stops at its login screen, and Remote Desktop needs the desktop to be running. So while Remote Desktop is to stay on, the phone starts its desktop by itself at boot, **locked from its first moment** (its lock screen asks for the PIN, in the browser or on the phone), and Remote Desktop starts with it. When Remote Desktop is off, the phone stops at its login screen as before.

Two things to know:

- **The internet address is a new one** after each restart (quick tunnel). The Wi-Fi address stays the same. A fixed internet address needs a Cloudflare account and a domain (`tunnel = token`)
- **The wallet** (KWallet) opens with the password typed at the login screen. After a restart without it, the first app that wants a saved secret asks for the password once

## How it works

```
browser (any computer)                          browser on the same Wi-Fi
   │  https, port 443 (WebSocket)                  │  https, port 7443, the phone's own certificate
Cloudflare  ◄── the phone connects out to it       │
   │                                               │
cloudflared (on the phone)                         │
   │  http, 127.0.0.1:7180                         │
s24-remote (Rust): the doors (code or password, cookie), the page, the pace
   │  frames on stdin/stdout
openandroiddex-pmos (C, from Open Android DeX): KWin's virtual screen, PipeWire, x264, KWin's fake input
```

| Piece | Where | Job |
|---|---|---|
| `s24-remote` | `src/remote/` (Rust: tokio, hyper, tungstenite, zbus), installed as `/usr/local/bin/s24-remote` | Runs in the desktop session while Remote Desktop is on. Serves the page and the WebSocket on 127.0.0.1 only, signs browsers in, starts the tunnel and the screen program, paces the video to the browser's link, switches the phone's screen, holds the sleep lock, owns `org.s24.Remote` on the session bus |
| The page | `src/remote/www/` (built into the program) | WebCodecs H.264 decoder, drawing on a canvas, mouse, keys, touch, the menu |
| `cloudflared` | Cloudflare's own program, 2026.10.7, downloaded and checked against its SHA-256 by the install script | The way in from the internet |
| `openandroiddex-pmos` | [Open Android DeX](https://github.com/dotnetdreamer/open-android-dex)'s `openandroiddex-pmos/`, installed as `/usr/local/libexec/s24-remote/openandroiddex-pmos` | Asks KWin for a screen, encodes it, types and points on it; also docked mode while it runs, apps opening on the screen they were opened from, the clipboard |
| The tile | `rootfs/usr/share/plasma/quicksettings/org.s24.quicksetting.remote/` | Watches `org.s24.Remote`; its `Summary` property is the line under the name |
| Login autostart | `rootfs/etc/xdg/autostart/s24-remote-cleanup.desktop` | `s24-remote login`: the phone's screen back if a session ended without doing it (a power cut, say), and Remote Desktop on again if it is to stay on |
| Automatic login | `rootfs/usr/local/libexec/s24-remote/autologin`, greetd's `[initial_session]` in `/etc/phrog/greetd-config.toml`, `rootfs/usr/local/share/s24-remote/autologin-xdg/kscreenlockerrc` | At boot, while Remote Desktop is to stay on: the desktop without the login screen, locked from the start (kscreenlocker's `LockOnStart`, for that session only). Otherwise it ends at once and greetd shows the login screen |
| The window script | `rootfs/usr/local/share/kwin/scripts/convergentwindows/` | Plasma Mobile's script of that name, decided per screen: phone windows on the phone's panel, desktop windows on the others. KWin finds it before Plasma Mobile's own in `/usr/share` |
| The second screen's panels | `src/remote/src/panels.rs` | A status bar and a navigation bar for screen 1, made once through plasmashell's scripting, so each of two screens has its own |
| The firewall rule | `rootfs/etc/nftables.d/50_s24remote.nft` | Port 7443 on Wi-Fi, USB and wired networks only. **This phone's kernel has no nftables**, so it does nothing here: `s24-remote` checks for itself (below) |

`s24-remote on | off | toggle | status | show` are what the tile and the notification run; `s24-remote password [--remove]` sets the password at the phone; `s24-remote login` is the login autostart; `s24-remote serve` is the program itself.

## Why it is built this way

**The tunnel.** A phone on mobile data has no address the internet can reach (the carrier's NAT), and neither does one behind a home router. Something has to connect out. Cloudflare's quick tunnel needs no account: cloudflared connects to Cloudflare, and Cloudflare gives it an address with a real certificate. Browsers reach it over https on port 443, the one door schools and offices leave open, and WebSockets go through. Tailscale and the like need an app on the computer too, which is what this avoids.

**The video.** The picture is H.264 from x264 on the phone's fast cores (the S24 has no hardware encoder Linux can use), decoded by the browser's WebCodecs, usually on the computer's GPU, and drawn the moment it leaves the decoder. No video element, no buffering. The phone turns the encoder's Annex B into the AVC form (each unit behind its length, the parameter sets sent once as the decoder's description) because every browser's WebCodecs takes that, and not all take Annex B. The pointer is the computer's own, so it moves with no delay whatever the video does.

**The pace.** Across the internet the danger is not a slow picture but a late one: frames sent faster than the link carries pile up in buffers, and everything you do shows seconds later. So every frame is acknowledged by the browser once it is drawn, and the phone only encodes a new frame while few enough are in flight (twice what the link's own round trip holds at the frame rate, plus two, at least 4 and at most 12). Frames are dropped before they are encoded, never after: a dropped encoded frame would break the chain of frames that refer to it. The frame rate is capped by spacing those acknowledgements, and the bit rate is cut when frames start to queue and raised again when the link is clear. Details in `src/remote/src/flow.rs`.

**The phone's own screen.** KWin has one screen-off state for all screens: the browser's screen and the phone's go dark together, and the first key or mouse move from either side lights both. A phone in a pocket would light up whenever the browser is used. So while a browser is connected the phone's panel is switched off as a screen (KWin moves everything to the browser's), and its touchscreen is held (a touch on a switched-off panel would land on the browser's screen). Holding it with `EVIOCGRAB` rather than through KWin's input settings matters: KWin saves "disabled" for the device, and a crash would leave the touchscreen dead after a restart; a grab ends with the process that holds it. If anything dies, KWin brings the panel back by itself as soon as the browser's screen is gone, because it keeps its screen settings per set of screens.

**Docked mode.** The screen program turns Plasma Mobile's docked mode on while the browser's screen is up (movable windows with title bars) and back off after, as it does for Open Android DeX on a computer and as `s24-dockd` does for a USB-C monitor. Apps opened from the browser's screen open there.

**The phone stays a phone.** Docked mode is one switch for the whole phone, and Plasma puts the session's panels on its first screen. After the phone's panel had been switched off and on beside the browser's screen, KWin made the browser's screen the first: the status bar and the navigation bar went there, and the phone was left with neither (no drawer to pull down, no gestures) and with desktop windows. Two things fix it. Plasma Mobile's window script is replaced by one that decides per screen: in docked mode, beside another screen, windows on the phone's own panel fill it without title bars, as in phone mode, and windows elsewhere are desktop windows. It reads docked mode from KWin's own window rules (centred placement in docked mode, maximizing on a phone), which Plasma Mobile's envmanager sets whenever the mode changes, because KWin's copy of the setting itself was seen to miss a change. And a second status bar and navigation bar are made once for screen 1, so each of two screens has its own whichever one KWin makes the first. A monitor beside the phone gets the same.

## Who gets in

- **Two doors, one sign-in.** The tunnel's door is on 127.0.0.1: the only way to it from outside is the tunnel. The Wi-Fi door is on port 7443, HTTPS with the phone's own certificate (key kept in `~/.local/share/s24-remote/`, readable by the user only)
- **The Wi-Fi door is for the local network only, and checks that itself:** this phone's kernel has no nftables, so there is no firewall at all. It listens on IPv4 only (on mobile data the phone can have an IPv6 address the whole internet reaches) and takes a connection only if it came to the phone's address on Wi-Fi, USB or a wired network, from a private address. On a school or office Wi-Fi, others on it can reach the sign-in page, as on the internet address; the code or password keeps them out
- **A code, once.** 8 digits from the kernel's random source, shown only on the phone, traded once for a 256-bit token the browser keeps in an `HttpOnly; Secure; SameSite=Strict` cookie. Tokens live only in the program's memory: turning Remote Desktop off signs everyone out
- **Guessing doesn't pay.** A wrong code takes a second, tries are taken one at a time from everywhere at once, 10 wrong codes replace the code, and after 3 codes sign-in closes for an hour (or until Remote Desktop is turned off and on). That is 30 tries an hour at a hundred million codes
- **The password**, if you set one: at least 8 characters, so the same 30 tries an hour get nowhere even if it is all digits. Kept only as a salted PBKDF2-SHA256 hash (150 000 rounds) in `~/.local/share/s24-remote/password`, readable by the user only. Wrong passwords count as wrong codes. Changing it needs a signed-in browser or the phone, and the phone tells you
- **The automatic login** (only while Remote Desktop is to stay on) starts the desktop locked, before anything is shown: kscreenlocker's own lock-on-start, and the login autostart locks it as well if it somehow isn't. Without it, a restart would end remote use until someone logs in at the phone
- **Requests must name our address** (`Host`), and anything that changes something, and the WebSocket, must come from our own page (`Origin`). The page may load nothing from elsewhere (`Content-Security-Policy`)
- **The phone tells you.** Each browser that signs in gets a notification on the phone, with where it came from
- **The phone's PIN still counts.** A locked phone shows its lock screen in the browser
- **One browser at a time.** A second one takes over; the first is told so

Cloudflare carries the traffic over TLS to the browser, and can see it, as any reverse proxy can. On a computer whose network inspects TLS (some workplaces), the network can too.

## Settings

`~/.config/s24-remote.conf`, `key = value`, all optional:

| Key | Default | |
|---|---|---|
| `tunnel` | `quick` | `quick`: a new trycloudflare.com address each time. `token`: a named Cloudflare tunnel with a fixed address (needs an account and a domain; also set `tunnel_token` and `hostname`, and point the tunnel at `http://localhost:7180`). `none`: no tunnel |
| `grace_minutes` | `5` | How long the desktop waits for a browser that went away |
| `phone_screen` | `off` | `on` keeps the phone's own screen on while a browser is connected (the page can switch it too) |
| `lan` | `on` | `off`: no Wi-Fi address, the tunnel only |
| `lan_port` | `7443` | The Wi-Fi address's port |
| `port` | `7180` | Where cloudflared reaches s24-remote, on 127.0.0.1 |
| `helper`, `cloudflared` | | Other paths for the two programs |

Log: `~/.cache/s24-remote/s24-remote.log`. Kept between runs: `~/.local/share/s24-remote/` (the Wi-Fi certificate and its key, the password's hash), `~/.local/state/s24-remote/keep-on` (there while Remote Desktop is to stay on).

## Installing

On the phone, from this repo: `sudo sh scripts/install-remote-desktop.sh [/path/to/openandroiddex-pmos]`. It builds `s24-remote` with cargo (about 90 s on the phone with nothing built yet, 40 s again), downloads and checks cloudflared, installs the screen program (the path given, or the copy the Open Android DeX app put in the user's home), the tile, the window script (and swaps it into a running KWin), the firewall rule, and the automatic login's pieces, puts the tile next to Mouse Mode, and adds the automatic login to greetd's config (`S24_REMOTE_NO_AUTOLOGIN=1` leaves greetd alone). The tile appears after the next login.

The screen program comes from Open Android DeX (`openandroiddex-pmos/build.sh` builds it in Docker). Version 6 works; version 7 lets Low quality go below 2 Mbit/s and converts only the frames it encodes.

Building `s24-remote` on a PC instead needs a C compiler for aarch64 Linux too (`ring`, the Wi-Fi door's and the password's cryptography, has C and assembly): building on the phone is simpler. The binary is about 3.4 MB.

## Measured

On the Galaxy S24, Wi-Fi at home, through a quick tunnel, the browser a headless Edge on a PC on the same network (so out to Cloudflare and back). Everything with windows in it (dragging, quality, clicks, CPU) was measured on the private nested test session on the phone. The real session was locked, so there only its lock screen was measured:

- Remote Desktop on: the address was ready about 5 s after it was turned on (from inside the real session, where the tile starts it). Sign-in to the first picture: about 1 s
- The phone on mobile data alone (LTE, an IPv6-only carrier with IPv4 through CLAT; Wi-Fi switched off for 150 s after the tunnel was up): cloudflared reconnected by itself and the same address kept working. The page answered in 0.43 s, sign-in worked and the picture came through (the lock screen: the phone was locked). One reading: network round trip 57 ms, a frame's 81 ms (sent, decoded, acknowledged)
- Medium, 1600x900, dragging a window around: 27 to 30 frames a second (the cap is 30), about 2 Mbit/s, a frame's round trip (sent, decoded, acknowledged) 30 to 60 ms, 1 or 2 frames in flight
- Low, the screen 1280x720: 19 to 20 frames a second (the cap is 20), about 0.2 Mbit/s for the same dragging
- High: 28 to 32 frames a second, 3.4 to 4.2 Mbit/s; the test session's KWin tops out around 37 frames a second
- A click to the changed button in the browser: 143 to 196 ms (5 clicks), in a nested test KWin that is slower than the real one
- The browser not acknowledging anything for 6 s: frames in flight stay at the window (4 or 5), and the picture is back at full rate within a second of it answering again
- CPU, a window animating on every frame at 1600x900, Medium, one 15 s reading each: `s24-remote` 1 to 3% of a core, the screen program 88% (5% with a still screen), KWin's test copy about 50%. An earlier build of the screen program, without version 7's two CPU changes, read 115% in the same test, but at a lower bit rate than the 88% run, so that pair is no clean comparison. Four colour-conversion threads against one, at about the same bit rate: 99% and 88%

The Wi-Fi door, the password and staying on (2026-11-20, version 1.1.0), on the real session, with a visible Edge window on a PC on the same Wi-Fi, driven over CDP:

- The Wi-Fi address: Edge's "not private" warning, **Advanced**, **Continue**, then the sign-in page as a secure page (`isSecureContext` true, WebCodecs there), sign-in with the code, the picture. Round trip about 20 ms (the link's own 6 ms), 2.9 to 3.3 Mbit/s on Medium
- The password: too short and two that don't match refused in the page; set; a wrong one refused ("That password isn't right."); the right one signed in on the Wi-Fi address and through the tunnel; removed (the file gone)
- Staying on: `s24-remote on` leaves the flag; Remote Desktop ended the way a logout ends it (SIGTERM), the flag stayed, and `s24-remote login` (what the login autostart runs) turned it on again by itself, with the same Wi-Fi certificate
- The automatic login's lock: a nested KWin started with its config folder in `XDG_CONFIG_DIRS`, the way the automatic login starts the desktop, was locked with nothing asking it to lock (twice); without the folder it wasn't (twice). The script itself, dry-run: ends at once without the flag; with it, Plasma Mobile's session with the login screen's environment
- The phone as a phone: in the real session, both screens had their own status bar and navigation bar (screen 0 the browser's, with Plasma Mobile's own; screen 1 the phone's, with the new pair). The window rules per screen were tested in the nested session over two rounds of every move: phone windows full screen without title bars beside the browser's screen, desktop windows on the browser's screen, and back

The tests (a private nested Plasma Mobile session on the phone for the desktop itself, the real session up to its lock screen and then unlocked by its owner, a browser driven over CDP through the real tunnel and the Wi-Fi door) are in [tests/remote/](../tests/remote/).

## Not done

- **Characters that aren't on the phone's keyboard layout** (`ä`, `é` with the phone on a US layout) arrive as a wrong key: the screen program falls back to KWin's own typing of a symbol, which delivers F24. Everything on the layout types correctly, `€` included
- **The pointer's shape** stays an arrow (KWin's screen is sent without the pointer, and its shape isn't passed on)
- **No sound** in the browser
- **The address changes** each time Remote Desktop is turned on, and after each restart (quick tunnel). The Wi-Fi address doesn't. A fixed internet address needs a Cloudflare account and a domain (`tunnel = token`)
- **A whole restart with the automatic login** hasn't been run: its pieces have (the lock at start, the script, the login autostart), the restart itself not yet
- **KWin moves windows back** to the phone's screen when it comes on again, if they were there before it went off
- **The phone's own gestures and status bar on screen 1** were checked as panels on the screen, not with a finger: touch on the phone's screen beside the browser's is the owner's to try
- **Networks that block it:** some filters block `trycloudflare.com` (the browser's side), and some networks block Cloudflare's tunnel port 7844 (the phone's side; mobile data doesn't). There is no fallback for proxies that refuse WebSockets
- **The lock screen broke once during testing.** On the test phone, locked the whole time, Plasma Mobile's lock screen process crashed four times and kscreenlocker went into its emergency mode ("The screen locker is broken and unlocking is not possible anymore"): no PIN pad until an unlock with `loginctl unlock-session` or a restart. It was fine at 00:16 and broken by 00:33; in between plasmashell was restarted with `--replace` (the old one crashed on its way out and KCrash started it again), Remote Desktop was turned off and on, and Wi-Fi went off. In a controlled test on a locked nested session, the parts of Remote Desktop that were tried crashed nothing (5 rounds of its notifications, 3 browser screens added and removed, the panel off and on twice; no browser was signed in, and the nested session has no touchscreen or power key to try). The one nested crash came after a plasmashell restart, with the lock screen failing to reach the shell's `/Mobile` object just before. The restart is the likeliest cause, but it isn't proven. Since then `s24-remote` doesn't ask kscreenlocker to lock a session it has found locked already (if it can't find out, it still asks; see LESSONS), and the install script no longer suggests restarting plasmashell
- **The browser needs WebCodecs with an H.264 decoder** (the page says so when it has none). Tested in Edge (Chromium) only, not Firefox or Safari. The touch keyboard and touch gestures (a tablet or a phone as the computer) and the Mac's Cmd-as-Ctrl are untested
- On the real session, tested up to the lock screen: unlocking a phone remotely is the owner's to do. The desktop itself (typing, clicks, dragging windows, docked mode, quality, resizing, taking over, stalls, the clipboard both ways) was tested in a private nested session on the phone
- **The tile and the power button by hand.** The tile was checked by loading its QML in `qmlscene` and calling its toggle; Remote Desktop was started from inside the session with `s24-remote on`; in the tests the power button's presses were written into its input device, the way `s24-powerd` sends them. Once, on 2026-11-20, the phone's power key was pressed while a browser was connected (s24-powerd logged it like a hand press): the phone locked and got its screen back, as it should. A finger on the tile is untested
