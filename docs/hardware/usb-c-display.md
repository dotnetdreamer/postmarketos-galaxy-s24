# USB-C monitors and docks

**State:** works. A USB-C monitor or dock shows the desktop as a second screen and charges the phone at the same time, with windows you can move (docked mode turns on by itself). The dock's USB ports work too: keyboard, mouse, wired network.

Tested with a Dell WD19DCS dock and a 2560x1440 monitor:

- Picture at 2560x1440, 75 Hz, as a second screen next to the phone's (KWin output `DP-1`)
- Charging at the same time (about 0.9 A into the battery)
- A Logitech keyboard and mouse on the dock: found and set up as input devices
- The dock's Realtek network chip: driver loaded, `eth0` appears. No cable was plugged in, so a real connection wasn't checked
- After a reboot with the dock plugged in, the monitor came back by itself (the boot script ran 32 s after power on)

## What was wrong

Charging worked from the start, but the screen stayed dark. The phone never asked the monitor for DisplayPort:

- The USB-C chip (Maxim MAX77775) only starts the DisplayPort handshake (the "alternate mode") when the kernel tells it to. At boot it is told "ready, but stopped". Samsung's USB event log `/proc/usblog` shows `ccic alternate mode is READY & STOP`
- `usb_typec_manager` sends "start" once four things are ready: the USB driver, the battery driver, the DisplayPort driver, and the end of Samsung's USB boot delay. The log showed `alt_is_support 7`: three of four. The boot delay never ended
- The boot delay ends in `usb_notify` (function `reserve_state_check`). It waits until Android reports the screen lock state in `/sys/class/usb_notify/usb_control/usb_sl`. The value starts at `3` ("not reported yet") and nothing in postmarketOS ever writes it
- The same wait kept the USB host role off, so nothing on the dock's USB ports showed up either

`usb_sl` values: `0` unlocked, `1` locked, `2` locked with new USB devices blocked (Samsung's lock screen USB protection), `3` not reported yet.

What did not help: writing `1` to `/sys/class/power_supply/battery/boot_completed`. The battery driver logs it and nothing else happens.

Samsung's kernel source for this phone helped a lot here: `drivers/usb/notify/usb_notify.c`, `drivers/usb/typec/manager/usb_typec_manager_notifier.c` and `drivers/usb/typec/maxim/max77775_alternate.c`.

## The fix

`rootfs/etc/local.d/s24-usbc.start` runs at boot:

- Writes `0` (unlocked) to `usb_sl`. That ends the boot delay, the manager sends "start", and the MAX77775 enters DisplayPort mode with any monitor plugged in then or later. There is no lock screen USB restriction, which is the same as plain Linux
- Loads the USB network drivers from `/usr/lib/modules/system_dlkm` (Realtek RTL8152/8153, ASIX AX88179/AX88772, the generic CDC ECM/NCM ones), because nothing loads them on demand on this system. NetworkManager connects a cable by itself

## Check it

```sh
cat /sys/class/usb_notify/usb_control/usb_sl   # 0
cat /sys/class/sec/ccic/usbpd_ids              # the dock's or monitor's USB ids, not 0000:0000
cat /sys/class/drm/card1-DP-1/status           # connected
grep -a "alternate mode is" /proc/usblog       # READY & START after the boot script ran
kscreen-doctor -o                              # in the desktop session: DP-1 enabled
```

`tests/usb-c/usbc-state.sh` prints all of it (run as root, it changes nothing).

To see what the monitor shows from a shell: `src/tools/fbgrab.c` on the plane that scans out 2560x1440 (try plane ids until one reports that size).

## Windows you can move (docked mode)

Plasma Mobile shows every app full screen without a title bar, on every screen, unless its **Docked mode** is on (the quick setting; `convergenceModeEnabled` in `plasmamobilerc`). So `rootfs/usr/local/bin/s24-dockd` turns it on while a monitor is connected and puts back what the phone had when the monitor goes, the same way Open Android DeX's postmarketOS helper does for a computer's screen:

- It runs in the desktop session (`rootfs/etc/xdg/autostart/s24-dock.desktop`, Plasma Mobile only) and listens for the kernel's DRM hotplug events; whether a monitor is there comes from `/sys/class/drm/card*-DP-*/status`
- Writing the setting is not enough: it also runs `plasma-mobile-envmanager --apply-settings`, as Plasma Mobile's own switch does, which gives KWin docked mode's rules (`InteractiveWindowMoveEnabled=true`, `Placement=Centered`, no borderless maximized windows in `~/.config/plasma-mobile/kwinrc`). Without it windows can't be dragged
- What the phone had is kept in `~/.cache/s24-dock/docked-before` until it is put back, so a restart while docked still puts it back at the next login. A phone that had docked mode on already is left alone
- Docked mode is one setting for all screens: while the monitor is connected, the phone's own screen has windows with title bars too
- Windows that were open before the switch keep their size; drag the title bar (or a browser's tab bar) to make them movable windows. New windows open centred
- Log: `~/.cache/s24-dock/s24-dockd.log`

Checked on the phone: started with the dock connected, it turned docked mode on, KWin got the docked rules, and an open dialog got a title bar. Not checked yet: unplugging and plugging back in (docked mode off and on again).

## Not done

- Sound through the monitor (DisplayPort audio): not tried
- The Realtek network chip asks for a firmware patch (`rtl_nic/rtl8153b-2.fw`) that the phone doesn't have. It works without it
- Not tested since this change: the phone on a PC's USB port (the rescue network on `usb0`). The fix lets Samsung's USB code finish its boot steps, which on Android also run with a PC attached
