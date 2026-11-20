# Lessons learned

Everything here was learned the hard way on this phone (Galaxy S24 SM-S921B, kernel 6.1.93-android14-11, firmware S921BXXU6BYE7). Most of it costs hours to rediscover.

## Never do this

| Don't | What happens |
|---|---|
| Install newer stock firmware | Newer releases can remove OEM unlocking; the bootloader can't be downgraded |
| Touch `efs`, `sec_efs`, `cpefs` without a backup | IMEI and radio calibration live there; there is no way to recreate them |
| `rmmod npu` | Kernel panic, full reboot. A fresh boot has it unloaded: just `insmod` it |
| Read `/sys/kernel/debug/gpio` | ITMON bus timeout, kernel panic |
| `echo processors > /sys/power/pm_test` (or `core`) | SError in the USB block, panic. `freezer`, `devices`, `platform` are fine |
| Load a self-built module that exports a symbol from a "protected" GKI module | Rejected, and the rejection path itself oopses: reboot (see Bluetooth below) |
| Load a module built against a kernel config that differs in any struct | Crashes in `mod_sysfs_setup` or later; check struct layouts first (`src/bluetooth/s24bt/cmp_layouts.py`) |
| Close a camera session while frames or results are still coming | Samsung's camera provider crashes and comes back half-initialised; every later open fails until a full setup |
| Overwrite a shared library that a running program has mapped (`gcc -o` straight into place) | That program crashes. Build to a temp file, then `mv` |
| Kill the phone's KWin to restart it | The wrapper respawns it but the shell is gone; log out and in instead |

## Boot

- **The init_boot ramdisk must be lz4 legacy** (`lz4 -l`). The bootloader appends it to the stock lz4-legacy vendor ramdisk; a gzip archive behind it is never unpacked, there's no `/init`, and the phone ends in Upload mode about 87 s later. This alone cost a day of "my init never runs"
- **`modules.load` is not in dependency order.** Load each module's `modules.dep` entries first, last one first (like Android's libmodprobe), and honour `modules.softdep`. Without softdeps: `exynos_drm` needs `samsung_dma_heap` first (else a NULL dereference at the first modeset), `mcd_panel` needs `s2dos05-regulator` and `i2c-exynos5` (else "panel disconnected")
- **The bootloader starts a hardware watchdog.** If nothing feeds it (`s3c2410_wdt` loaded, `/dev/watchdog` fed every 10 s), the phone resets about 87 s after boot. Looks exactly like the ramdisk not running
- **Any reboot before `sec_reboot` is loaded lands in Upload mode** (Samsung's crash dump mode), whatever the reason. Upload mode's dump protocol never answered on this phone
- **The panel id comes as a bare `lcdtype=` on the kernel command line**, but `mcd_panel` only reads it as its `boot_panel_id` module parameter: pass it at `insmod`, or you get a default panel table
- **No devtmpfs, no uevent helper**: `mdev -s` in a loop makes the nodes. mdev matches the whole device name including subdirectories (`(dri/)?renderD[0-9]+`). Without an `/etc/mdev.conf` in the ramdisk, the loop resets every node to `0660 root` and dbus dies on `/dev/null`
- **Firmware is loaded relative to the root of PID 1's original filesystem** (the stage-0 ramdisk), not the chrooted postmarketOS root: bind `/newroot/vendor` onto `/vendor` in the ramdisk
- Hand PID 1 over with `chroot`, not `switch_root`: stage 0's telnet and device loop then keep running as a rescue path
- A cloned git tree on Windows can have CRLF module lists (`core.autocrlf`): busybox then loads nothing. Strip `\r`
- Logs from the previous boot: `/proc/last_kmsg`; a multi-boot ring: `/proc/dss_kmsg` (take the text after the last "Booting Linux"); why it reset: `/proc/reset_reason`
- Holding Power + Volume Down is a deliberate kernel panic in Samsung's kernel ("upload cause 0x66"): it looks like a crash in `last_kmsg` but is a forced restart

## Kernel limits (Samsung's GKI config)

- No `DEVTMPFS`, `VT`, `FB`, `FHANDLE`, `USER_NS`, `PID_NS`, `SYSVIPC`, `POSIX_MQUEUE`, `CGROUP_PIDS`, `CGROUP_DEVICE`, `NF_TABLES`. So: no systemd, no rootless containers, no Flatpak sandbox, no nftables (use iptables-legacy)
- `MODULE_SIG_PROTECT`: unsigned modules load, but may not export any symbol that a Samsung-signed "protected" module exports
- Many kernel threads (`tz_worker`, `dsms`) sit in D state: load average about 16 while idle is normal

## Display and graphics

- `exynos-drmdpu` ignores a framebuffer's pitch and uses width x bytes-per-pixel. The GPU's linear pitch is 256-byte aligned (1080 px gives 4352 bytes): register framebuffers 1088 wide, keep the plane source 1080 (`libs24fbfix.so`)
- The display planes advertise only Samsung modifiers, never LINEAR, yet scan linear fine. KWin intersects them with Zink's list, finds nothing, and the screen stays black. `libs24kwin.so` adds LINEAR to what KWin reads
- `/sys/class/drm/*/dpms` always says Off. The real panel state: `/sys/class/drm/card1-DSI-1/enabled`, or the `DSI-1` block of `/sys/kernel/debug/dri/<n>/state`
- GTK with `GSK_RENDERER=ngl` (GL through Zink) reset the phone; `GSK_RENDERER=vulkan` works
- KWin re-opens the DRM device by path: the user must be in group `video`
- Wayland compositor tests need `dbus-run-session`, or they wait forever for a bus and look hung
- `fbgrab` (`src/tools/fbgrab.c`) captures whatever the panel shows, including KWin's 10-bit formats

## Wi-Fi, USB, charging

- Wi-Fi order (as Android's `init.insmod.sh`): cnss modules, `rfkill`, `cfg80211`, `kiwi_v2`, then `echo 1 > /sys/devices/platform/qcom,cnss-kiwi/fs_ready`. Calibration takes about 25 s
- Samsung's battery driver limits a PC's USB port to 100 mA until "its" USB stack reports the gadget configured: write `850` (SuperSpeed) or `475` to `/sys/class/power_supply/battery/usb_conf_test` when the UDC state turns `configured`
- Windows binds its ADB driver to Google's `18d1:d001` gadget IDs; `1d6b:0104` with an IAD gets NCM + serial
- Every real modem power change (on to off or back) makes the PC reset the phone's USB link: TCP over the cable drops once. Run such commands detached and reconnect
- A suspend while USB data is up leaves dwc3 dead; with the cable in, `usb_notify` and `dwc3-otg` hold wakeup sources, so a correct wakeup-count handshake simply doesn't sleep (like Android)
- **A USB-C monitor only charged the phone until something reported the screen lock state.** Samsung's `usb_notify` ends its USB boot delay only after Android writes the lock state to `/sys/class/usb_notify/usb_control/usb_sl` (boot value `3`, "not reported"). Until then DisplayPort alternate mode stays stopped and the USB host role stays off. Write `0` (unlocked) at boot. `/proc/usblog` (Samsung's USB event history, survives the kernel log filling up) shows the whole chain: look for `alternate mode is` and `alt_is_support` (15 = all four parts ready)
- Writing `boot_completed` to the battery driver does not end that boot delay, despite the name

## Sound

- The audio DSP (ABOX "Calliope" firmware) drops any stream whose period is over 20 ms: use 10 ms periods, no timer scheduling, no mmap (`tsched=0`, `mmap=false`; `aplay --period-size=480 --buffer-size=1920`)
- It books every `write()` as one whole period. PulseAudio writes partial periods when it resamples, the DSP counts them twice, overflows and mutes: audible stutter. Fix: `src/audio/pa-whole-periods.patch` (write whole periods only)
- Real-time scheduling in PulseAudio is impossible on musl (`_POSIX_PRIORITY_SCHEDULING` undefined); rtkit still gives nice -11
- `/proc/abox/log-00` (the DSP's own log) blocks like a pipe: run one `cat` in the background and kill it
- A silent end-to-end test: with both amps muted, the amp DSP's "Protection AUDIO_IN_MAX" control still shows the signal arrived
- Busybox `patch` silently skips a hunk with a wrong line count (no `.rej`): check the result

## Bluetooth

- Samsung stubbed out `net/bluetooth/hci_sock.c` in their kernel (Android doesn't use kernel Bluetooth sockets): BlueZ panics the kernel. And `bluetooth.ko` is a protected module, so a rebuilt one that exports symbols is rejected
- Way out: one module (`s24bt.ko`) with the Bluetooth core, RFCOMM, hci_uart and btqca linked in and **no exports at all**
- Before loading any self-built module: `struct module` size must match (0x440 here), no `__ksymtab`, same vermagic, every import present in `/proc/kallsyms`, and every shared struct identical (BTF of the running kernel vs the module's DWARF). A tree built without `DEBUG_INFO_BTF_MODULES` or with a different `LOCALVERSION` handling had a 0x400 `struct module` and crashed
- The WCN7851 firmware always uses Qualcomm in-band sleep, whatever the NVM says: attach with protocol `HCI_UART_QCA` (8 in this tree), not H4
- `btpower`'s first chip start after `insmod` holds a wakeup source forever: start once, stop, start again
- `s24bt` can't be unloaded (RFCOMM holds its own module): a new build needs a reboot

## Modem (Samsung RIL in a container)

- Container traps, each fixed in `android-env.sh` / `android-ctr0.sh` / `init.s24.rc`: no PID namespace (so Android init isn't PID 1 and refuses cgroups: hand-written `/dev/cgroup_info/cgroup.rc`), services need an explicit `seclabel`, init's tmpfs hides `/apex` (bind them back in early-init), linker config generated with a hand-written `apex-info-list.xml`, service managers need an SELinux shim, **one binderfs instance** shared by both worlds (a second mount is a separate binder universe)
- rild waits for `android.system.suspend.ISystemSuspend/default`: run `android.system.suspend-service`
- `libgbinder` needs `aidl3` for both protocol and service manager on this Android 15 build (`/etc/gbinder.conf`)
- AIDL method codes are the declaration order (index + 1) in the frozen `aidl_api` files; check the interface hash first (`aidlver`)
- **The 2G trap**: Samsung's RIL drops the initial attach APN when it doesn't know the SIM operator. Android sets `gsm.sim.operator.numeric` first. Without it the modem stays on EDGE
- rild crashes when handed binders that are already dead; a rild that restarted while the modem kept running answers only "Invalid response: NULL": restart the modem processor
- rild answers only the last registered client: run one client (`s24-mmd`), never two
- NetworkManager can't own `rmnet0` itself; `s24-mmd` provides ModemManager's D-Bus API so NM, Plasma and GNOME Calls work unchanged
- IPv6-only carriers: IPv4 comes from clatd + TAYGA; the hook pins the DNS64 prefix and keeps NM's DNS when Wi-Fi is up

## Sensors and power

- Sensor hub: `echo 1 > /sys/class/sensors/ssp_sensor/fs_ready` boots it; sensors are enabled with an ioctl on `/dev/batch_io` and read from IIO devices (details in [hardware/sensors-and-power.md](hardware/sensors-and-power.md))
- Double tap to wake: `aot_enable,1` on `/sys/class/sec/tsp/cmd`; the touch chip sends `KEY_WAKEUP`. **KWin ignores `KEY_WAKEUP`** while the screen is off on purpose; answer it with an injected power key
- Deep sleep needs the wakeup-count handshake (read `/sys/power/wakeup_count`, write it back, then `mem`); logind's own suspend skips it and NetworkManager drops Wi-Fi afterwards
- Wi-Fi wake-on-packet works and wakes the phone often on a busy network

## Camera

The long version is in [hardware/camera.md](hardware/camera.md). The short one:

- Linux camera apps can't see this camera: there is no libcamera pipeline for Exynos and no standard V4L2 sensor device. The only working route is Samsung's HAL, then our own PipeWire source
- Samsung's camera HAL needs: Samsung's **thermal and HyPer services running** (without them it half-initialises and crashes in ever-changing places), **cameraserver to prime it once** (only that builds its full setup), a **sensor service with a gyroscope** (it aborts without), the **sensor kernel modules** (only in the stage-0 ramdisk), and **SIGPIPE ignored**
- It returns `bufferId` 0 in results: track which buffer you sent for each frame
- A clean close plus reopen needs no new priming; a client killed without closing leaves it broken
- PipeWire buffers need a `SPA_META_Header` with real timestamps, or GStreamer's sink drops every frame as late and the app freezes after a few frames
- GNOME Snapshot mirrors the preview of any camera whose `api.libcamera.location` isn't `back`
- The camera's AI pipeline wants the NPU (`/dev/vertex10`); never unload it

## Desktop

- Chromium-based apps (Chromium, Brave, VS Code, Podman Desktop) refuse to get narrower than 430 to 670 px; the phone is 393 px wide at scale 2.75. On Wayland `--force-device-scale-factor` multiplies the compositor scale: 0.75 fits exactly. `s24-fit` picks the factor only while the phone screen is the only screen
- GTK apps have no per-app scale: `s24-fit run` puts them in a nested KWin with its own scale
- `kwriteconfig6 --delete` with `XDG_CONFIG_DIRS` set writes a `[$d]` key that masks the system default: Plasma's on-screen keyboard disappeared that way. Write the value explicitly instead
- Saying Yes to "IBus Preferences" makes KWin use IBus as its input method and hides Plasma's keyboard
- Samsung's touchpad device and the sensor hub's `meta_event` device show up as pointers in KWin: disable them through KWin's D-Bus input devices

## Remote Desktop

The long version is in [remote-desktop.md](remote-desktop.md). The short one:

- KWin has **one screen-off state for all screens** (Workspace::requestDpmsState): a virtual screen goes dark with the phone's panel, and the first key or pointer move from either side lights both. Its input filter swallows that first event. To keep the phone dark while a browser uses the desktop, switch the panel off as a screen (`kscreen-doctor output.DSI-1.disable`), not with DPMS
- A newly added or re-enabled screen turns every screen on: the panel lights for a moment whenever it comes back
- KWin keeps screen settings **per set of screens** (`kwinoutputconfig.json`): a panel switched off beside a virtual screen comes back by itself when that screen goes, and goes off by itself when it comes again. Code that switches it must expect to find it off already
- A touch on a switched-off panel lands at (0, 0) of KWin's space (`devicePointToGlobalPosition` finds no screen): hold the touchscreen with `EVIOCGRAB`. Don't disable it through KWin's D-Bus `InputDevice.enabled`: KWin saves that in `kcminputrc`, and a crash would leave a phone with a dead touchscreen after a restart
- logind lets a program block sleep (`Inhibit("sleep", …, "block")`) only from the desktop session itself: from SSH it is `AccessDenied`. elogind tells sessions apart by cgroup (`/sys/fs/cgroup/c3`); a test that must start something as the session would has to join it first
- Plasma's "turn off the screen when locked" ignores idle inhibitions: an idle lock screen goes dark after about a minute, and a pointer move wakes it
- A new quick tunnel address isn't known for its first seconds. Ask the phone's usual DNS (often the home router, which the computer asks too) that early and "no such name" can stay in its cache for minutes: try it over DNS-over-HTTPS
- Plasma Mobile loads quick settings once, when plasmashell starts, and adds unknown ones to the end of the drawer by itself
- The screen program's colour conversion with 4 threads cost more CPU than with 1 (99% against 88% of a core at 1600x900, 28 to 29 frames a second, about the same bit rate; one 15 s reading each)
- **Don't restart plasmashell while the session is locked.** `plasmashell --replace` makes the old instance crash on its way out (KCrash then starts it again), and Plasma Mobile's lock screen depends on the shell's `/Mobile` D-Bus object. On the test phone the lock screen process crashed four times in a stretch that included such a restart (and other changes), and kscreenlocker's emergency mode followed: no PIN pad until `loginctl unlock-session` or a restart. In a nested session one crash followed a restart. The likeliest cause, not a proven one. kscreenlocker counts crashes from the last unlock, so crashes that it quietly recovered from add up
- kscreenlocker's `Lock` on a session that is locked already sends SIGUSR1 to its lock screen process by pid; in emergency mode that process is gone, the pid is 0, and `kill(0, …)` signals KWin's whole process group. Check `GetActive` first
- **Plasma puts a session's panels on its first screen**, and KWin decides which screen is first. After the phone's panel was switched off and on beside a virtual screen, the virtual screen was first: the status bar and the navigation bar went there, and the phone had neither (no drawer, no gestures). A second pair made through plasmashell's scripting (`new Panel(...)`, then `panel.screen = 1`, which moves it with `setScreenForContainment`) gives each of two screens its own whichever is first; with one screen the second pair isn't shown. `kscreen-doctor output.NAME.priority.1` didn't change which screen was first in a nested KWin
- **Docked mode is one switch for every screen**, and only KWin's side changes with it (window rules and Plasma Mobile's `convergentwindows` script); the panels and the home screen don't look at it. A replacement `convergentwindows` in `/usr/local/share/kwin/scripts` (searched before `/usr/share`) can decide per screen
- KWin's copy of Plasma Mobile's docked-mode setting (`ShellSettings.Settings.convergenceModeEnabled` in a KWin script) missed changes written with `kwriteconfig6 --notify`, in a nested KWin, both ways. KWin's own window rules (`Options.placement`: Centered in docked mode, Maximizing on a phone), which `plasma-mobile-envmanager --apply-settings` writes and has KWin reload, didn't. Whatever switches docked mode runs the envmanager (the quick setting, `s24-dockd`, Open Android DeX's screen program)
- KWin keeps a QML script compiled by its file's URL: loading a changed file again under the same path runs the old version until KWin restarts. Unloading a script leaves its signal connections on windows behind, calling into a script that is gone
- `workspace.sendClientToScreen` took a nested KWin 6.8 down every time; setting the window's `frameGeometry` didn't
- **This kernel has no nftables, so the phone has no firewall at all**: `nft` fails ("cache initialization failed"), the nftables service is stopped, iptables has only its nat table. Whatever listens on every address can be reached from every network, mobile data's IPv6 address included: a server for the local network has to check for itself where a connection came from and where it went
- A page reached past Chrome's or Edge's certificate warning ("Continue") is a secure context: WebCodecs, cookies with `Secure`, WebSockets to the same address all work
- greetd's `[initial_session]` runs once a boot, as the user, without a password; when it ends, greetd starts its `default_session` (the login screen). A command that exits at once is "no automatic login this time"
- kscreenlocker's `[Daemon] LockOnStart=true` locks a session from its start. Read through `XDG_CONFIG_DIRS`, it can be given to one session only: `startplasmamobile` keeps what it finds there, after its own folders

## Working on the phone

- With the screen off and no cable, the phone suspends between your SSH commands: hold `/sys/power/wake_lock` inside every script
- `sudo sh -c` keeps the session's `LD_PRELOAD` (the graphics shims): container binaries then fail to link. `unset LD_PRELOAD` first
- The container's log clock is UTC; postmarketOS shows local time
- `pgrep -f` patterns can match your own `sudo`/`ssh` command line and kill your own script: anchor them (`^/usr/local/sbin/...`)
- Stage 0's `/tmp` is a ramdisk: a reboot wipes test scripts kept there
