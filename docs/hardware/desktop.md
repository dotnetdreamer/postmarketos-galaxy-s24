# Desktop: Plasma Mobile, apps, mouse mode, containers

## Plasma Mobile

**State:** works on the phone's screen (KWin 6.8). Phosh also works; pick the session at the login screen.

What it needed beyond the GPU setup ([display-and-gpu.md](display-and-gpu.md)):

- The user in group `video` (KWin re-opens the DRM device by path)
- `rootfs/etc/profile.d/s24-display.sh`: KWin's devices and the two graphics shims
- Two input devices that KWin wrongly treats as pointers (Samsung's touchpad device and the sensor hub's `meta_event`): disable them via KWin's D-Bus input device objects, so no mouse pointer shows on the touchscreen
- `s24-greeter-reaper` (service): the login screen leaves about 35 processes behind after every login; this kills them once the greeter session has ended
- The on-screen keyboard is `plasma-keyboard`. Don't accept "IBus Preferences" (it replaces the keyboard); `~/.local/share/applications/org.freedesktop.IBus.Setup.desktop` hides it in Plasma
- GTK apps' window buttons per session: `rootfs/home/user/.config/autostart/s24-gtk-buttons-*.desktop`
- Docked mode (movable windows) while a monitor is on the USB-C port: `rootfs/usr/local/bin/s24-dockd`, see [usb-c-display.md](usb-c-display.md)
- Log out cleanly with `qdbus6 org.kde.Shutdown /Shutdown org.kde.Shutdown.logout` (session bus); killing KWin leaves no shell

## Apps that are too wide for the phone

The phone is 393 logical pixels wide (1080 px at scale 2.75). Chromium-based apps never get narrower than their minimum: Chromium and Brave 500, VS Code 432, Podman Desktop 672.

- `rootfs/usr/local/bin/s24-fit flag MINWIDTH` prints `--force-device-scale-factor=F` while the phone's screen is the only one and is narrower than MINWIDTH (on Wayland the factor multiplies the screen scale; 0.75 fits 500 into 393 exactly). Used by `/etc/chromium/s24-phone-fit.conf` and the `brave`, `code`, `podman-desktop` launchers
- GTK has no per-app scale: `s24-fit run MINWIDTH CMD` runs the app in a nested KWin with its own scale, plus `libs24fit.so` (`src/desktop/libs24fit.c`). Example: `rootfs/usr/local/share/applications/gnome-system-monitor-kde.desktop`
- Firefox with `mobile-config-firefox` fits on its own

## Desktop apps that work

| App | How |
|---|---|
| Chromium | Alpine package; GPU through the system Mesa (`/etc/chromium/s24-gpu.conf`) |
| Firefox ESR | Alpine package + `mobile-config-firefox` |
| VS Code (official) | Microsoft's linux-arm64 tarball in `/opt/vscode`, run natively on glibc from the Flathub freedesktop 25.08 runtime (Flatpak itself can't run: no user or PID namespaces), GPU through the runtime's GL extension and a glibc build of the Xclipse driver. See `rootfs/usr/local/bin/code` and `rootfs/etc/glibc-ld.so.conf` |
| Brave | The official linux-arm64 zip in `/opt/brave` (check its signed checksum), same glibc and GPU setup |
| Podman Desktop | The official linux-arm64 tarball in `/opt/podman-desktop`, talks to the root Podman service |
| VLC | Alpine package; through Xwayland it needs `QT_SCREEN_SCALE_FACTORS` (see `rootfs/home/user/.local/share/applications/vlc.desktop`) |
| Resources, GNOME System Monitor | Alpine packages (task managers) |

Chromium-based apps draw some shapes wrong with GPU rasterization on this GPU stack, so the launchers turn GPU rasterization and accelerated 2D canvas off (scrolling, compositing and WebGL stay on the GPU).

## Mouse mode

A quick-settings tile ("Mouse Mode") for big desktop apps: the touchscreen becomes a touchpad, the screen scale drops from 2.75 to 1.5, and KWin's zoom shows it at 1.83x, panning when the pointer reaches an edge (like TeamViewer's mouse mode). Swipe down from the top edge to leave it.

- `src/desktop/s24-mouse-moded.c` (root, service `s24-mouse-mode`): grabs the touchscreen and re-emits it as a uinput touchpad (tap to click, two fingers to scroll or right-click, three-finger pinch to zoom); only grabs while no finger is down and the screen is lit
- `rootfs/usr/local/bin/s24-mouse-mode` (session, Python): owns `org.s24.MouseMode`, saves and restores scale, zoom and keyboard mode
- `rootfs/usr/local/share/plasma/quicksettings/org.s24.quicksetting.mousemode/`: the tile
- `src/desktop/s24-output-watch.c`: tells the daemon when the screen turns off (KWin's DPMS protocol)
- Tests: `tests/mousemode/` (a stand-in touchscreen, nothing reaches the real session)

## Containers

`podman` works as root with host PID, IPC and network namespaces (the kernel lacks PID, IPC and user namespaces): `rootfs/etc/containers/containers.conf.d/s24.conf`. Rootless Podman, Flatpak sandboxes and Waydroid's usual setup are not possible on this kernel. `s24-podman-api` provides the API socket for Podman Desktop (group `user` may use it), `s24-podman-nat` the NAT rules (iptables-legacy).

## Hidden-screen testing

To test a GUI app without showing anything on the phone: run `kwin_wayland --virtual --socket NAME --width 1080 --height 2340` on a private D-Bus, start the app on that socket, and capture with KWin's ScreenShot2 interface. Details in [../DEVELOPING.md](../DEVELOPING.md).
