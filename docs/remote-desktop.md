# Remote Desktop: the phone's desktop in any browser

**State:** works. Turn on **Remote Desktop** in the drawer, open the address it shows in any browser on any computer, wherever the phone and the computer have internet (tested with the phone on home Wi-Fi and on mobile data alone), type the code, and the phone's desktop is there: a screen of its own in docked mode, with windows you can move, your mouse and your keyboard. Nothing to install on the computer, no account, no VPN app. **Low**, **Medium** and **High** quality are at the top of the page; Medium is the default.

## Using it

1. Pull down the drawer and tap **Remote Desktop**. In a few seconds a notification shows the address (`https://<four words>.trycloudflare.com`) and an 8-digit code. The tile shows the code too. Long-press the tile to see the notification again; its **Copy link** puts the address, code included, on the phone's clipboard
2. On the computer, open the address and type the code. The code works once: the phone shows a new one for the next browser
3. If the phone is locked, the browser shows the lock screen with a note: type the phone's PIN there
4. Move the mouse to the top middle of the page (or tap the little bar there) for the menu:
   - **Low / Medium / High**: see below
   - **Full screen**: in Chrome and Edge, keys like Esc and Alt+Tab then go to the phone
   - **Keyboard**: brings up the on-screen keyboard on a tablet or phone
   - **Clipboard**: copy on one side, paste on the other (the browser asks once for permission)
   - **Phone screen**: whether the phone's own screen stays on while you are connected (off by default)
   - **Info**: frames a second, speed and delay
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

- The phone's own screen is off and its touchscreen ignores touches, so the phone can stay in a pocket. **The power button gives the phone back**: one press locks it and turns its screen on; another press turns the phone's screen off again, and the browser's stays on
- The phone doesn't sleep while Remote Desktop is on (a sleeping phone is off the internet, and nothing a browser sends wakes it). That costs battery: turn it off when you don't need it
- When the browser goes (closed, or the connection drops), the desktop waits 5 minutes for it with the windows where they were. After that the phone is locked, and its screen comes back but stays dark

## How it works

```
browser (any computer)
   │  https, port 443 (WebSocket)
Cloudflare  ◄── the phone connects out to it, so no open port, no public address needed
   │
cloudflared (on the phone)
   │  http, 127.0.0.1:7180
s24-remote (Rust): the door (code, cookie), the page, the pace
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
| Login cleanup | `rootfs/etc/xdg/autostart/s24-remote-cleanup.desktop` | `s24-remote cleanup`: the phone's screen back if a session ended without doing it (a power cut, say) |

`s24-remote on | off | toggle | status | show` are what the tile and the notification run; `s24-remote serve` is the program itself.

## Why it is built this way

**The tunnel.** A phone on mobile data has no address the internet can reach (the carrier's NAT), and neither does one behind a home router. Something has to connect out. Cloudflare's quick tunnel needs no account: cloudflared connects to Cloudflare, and Cloudflare gives it an address with a real certificate. Browsers reach it over https on port 443, the one door schools and offices leave open, and WebSockets go through. Tailscale and the like need an app on the computer too, which is what this avoids.

**The video.** The picture is H.264 from x264 on the phone's fast cores (the S24 has no hardware encoder Linux can use), decoded by the browser's WebCodecs, usually on the computer's GPU, and drawn the moment it leaves the decoder. No video element, no buffering. The phone turns the encoder's Annex B into the AVC form (each unit behind its length, the parameter sets sent once as the decoder's description) because every browser's WebCodecs takes that, and not all take Annex B. The pointer is the computer's own, so it moves with no delay whatever the video does.

**The pace.** Across the internet the danger is not a slow picture but a late one: frames sent faster than the link carries pile up in buffers, and everything you do shows seconds later. So every frame is acknowledged by the browser once it is drawn, and the phone only encodes a new frame while few enough are in flight (twice what the link's own round trip holds at the frame rate, plus two, at least 4 and at most 12). Frames are dropped before they are encoded, never after: a dropped encoded frame would break the chain of frames that refer to it. The frame rate is capped by spacing those acknowledgements, and the bit rate is cut when frames start to queue and raised again when the link is clear. Details in `src/remote/src/flow.rs`.

**The phone's own screen.** KWin has one screen-off state for all screens: the browser's screen and the phone's go dark together, and the first key or mouse move from either side lights both. A phone in a pocket would light up whenever the browser is used. So while a browser is connected the phone's panel is switched off as a screen (KWin moves everything to the browser's), and its touchscreen is held (a touch on a switched-off panel would land on the browser's screen). Holding it with `EVIOCGRAB` rather than through KWin's input settings matters: KWin saves "disabled" for the device, and a crash would leave the touchscreen dead after a restart; a grab ends with the process that holds it. If anything dies, KWin brings the panel back by itself as soon as the browser's screen is gone, because it keeps its screen settings per set of screens.

**Docked mode.** The screen program turns Plasma Mobile's docked mode on while the browser's screen is up (movable windows with title bars) and back off after, as it does for Open Android DeX on a computer and as `s24-dockd` does for a USB-C monitor. Apps opened from the browser's screen open there.

## Who gets in

- **The door is on 127.0.0.1.** The only way in from outside is the tunnel
- **A code, once.** 8 digits from the kernel's random source, shown only on the phone, traded once for a 256-bit token the browser keeps in an `HttpOnly; Secure; SameSite=Strict` cookie. Tokens live only in the program's memory: turning Remote Desktop off signs everyone out
- **Guessing doesn't pay.** A wrong code takes a second, tries are taken one at a time from everywhere at once, 10 wrong codes replace the code, and after 3 codes sign-in closes for an hour (or until Remote Desktop is turned off and on). That is 30 tries an hour at a hundred million codes
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
| `port` | `7180` | Where cloudflared reaches s24-remote, on 127.0.0.1 |
| `helper`, `cloudflared` | | Other paths for the two programs |

Log: `~/.cache/s24-remote/s24-remote.log`.

## Installing

On the phone, from this repo: `sudo sh scripts/install-remote-desktop.sh [/path/to/openandroiddex-pmos]`. It builds `s24-remote` with cargo (about 70 s on the phone), downloads and checks cloudflared, installs the screen program (the path given, or the copy the Open Android DeX app put in the user's home) and the tile, and puts the tile next to Mouse Mode. The tile appears after the next login.

The screen program comes from Open Android DeX (`openandroiddex-pmos/build.sh` builds it in Docker). Version 6 works; version 7 lets Low quality go below 2 Mbit/s and converts only the frames it encodes.

Building `s24-remote` on a PC instead: `rustup target add aarch64-unknown-linux-musl`, then `cargo build --release --target aarch64-unknown-linux-musl` with `CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=rust-lld` and `RUSTFLAGS="-C linker-flavor=ld.lld -C target-feature=+crt-static"`: a static 2.4 MB binary, no C compiler needed.

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

The tests (a private nested Plasma Mobile session on the phone for the desktop itself, the real session up to its lock screen, a headless browser driven over CDP through the real tunnel) are in [tests/remote/](../tests/remote/).

## Not done

- **Characters that aren't on the phone's keyboard layout** (`ä`, `é` with the phone on a US layout) arrive as a wrong key: the screen program falls back to KWin's own typing of a symbol, which delivers F24. Everything on the layout types correctly, `€` included
- **The pointer's shape** stays an arrow (KWin's screen is sent without the pointer, and its shape isn't passed on)
- **No sound** in the browser
- **The address changes** each time Remote Desktop is turned on (quick tunnel). A fixed one needs a Cloudflare account and a domain (`tunnel = token`)
- **Networks that block it:** some filters block `trycloudflare.com` (the browser's side), and some networks block Cloudflare's tunnel port 7844 (the phone's side; mobile data doesn't). There is no fallback for proxies that refuse WebSockets
- **The lock screen broke once during testing.** On the test phone, locked the whole time, Plasma Mobile's lock screen process crashed four times and kscreenlocker went into its emergency mode ("The screen locker is broken and unlocking is not possible anymore"): no PIN pad until an unlock with `loginctl unlock-session` or a restart. It was fine at 00:16 and broken by 00:33; in between plasmashell was restarted with `--replace` (the old one crashed on its way out and KCrash started it again), Remote Desktop was turned off and on, and Wi-Fi went off. In a controlled test on a locked nested session, the parts of Remote Desktop that were tried crashed nothing (5 rounds of its notifications, 3 browser screens added and removed, the panel off and on twice; no browser was signed in, and the nested session has no touchscreen or power key to try). The one nested crash came after a plasmashell restart, with the lock screen failing to reach the shell's `/Mobile` object just before. The restart is the likeliest cause, but it isn't proven. Since then `s24-remote` doesn't ask kscreenlocker to lock a session it has found locked already (if it can't find out, it still asks; see LESSONS), and the install script no longer suggests restarting plasmashell
- **The browser needs WebCodecs with an H.264 decoder** (the page says so when it has none). Tested in Edge (Chromium) only, not Firefox or Safari. The touch keyboard and touch gestures (a tablet or a phone as the computer) and the Mac's Cmd-as-Ctrl are untested
- On the real session, tested up to the lock screen: unlocking a phone remotely is the owner's to do. The desktop itself (typing, clicks, dragging windows, docked mode, quality, resizing, taking over, stalls, the clipboard both ways) was tested in a private nested session on the phone
- **Nothing was pressed by hand.** The tile was checked by loading its QML in `qmlscene` and calling its toggle; the real session's Remote Desktop was started from inside the session with `s24-remote on`; the power button's presses were written into its input device, the way `s24-powerd` sends them. A finger on the tile in the drawer and a real press of the power button are untested
