# postmarketOS on the Samsung Galaxy S24 (Exynos)

Real Linux (postmarketOS with Plasma Mobile or Phosh) running on the Galaxy S24 **SM-S921B** (Exynos 2400, codename `e1s`), on Samsung's own kernel. No Android underneath, except a small sealed-off container that runs Samsung's modem and camera drivers.

This repository has everything that made it work: the boot ramdisk, the postmarketOS device package, every service and fix installed on the phone, the source code of the helper programs, the test scripts, and the notes on what went wrong and why.

> **Warning:** This is not a one-click installer. It needs an unlocked bootloader (that permanently trips Samsung Knox and wipes the phone), a Linux PC or container, and patience. A mistake can leave the phone unable to boot until you reflash stock firmware. Only try it on a phone you can afford to lose.

## What works

| Part | State | Notes |
|---|---|---|
| Boot | Works | Stock kernel and stock vendor modules, our own `init_boot` ramdisk, about 40 s to the login screen |
| Screen and touch | Works | 1080x2340, Plasma Mobile and Phosh both run |
| Graphics (GPU) | Works | Xclipse 940 through the open radv-xclipse Vulkan driver, OpenGL through Zink |
| Wi-Fi | Works | Reconnects after reboot, wakes the phone from sleep on traffic |
| Charging | Works | Including from a PC's USB port (Samsung's 100 mA cap is lifted) |
| Speaker and microphone | Works | PulseAudio, with a small patch for Samsung's audio DSP |
| Bluetooth | Works | Scanning works and it survives sleep; pairing with real headphones not tested yet |
| Mobile data | Works | LTE through ModemManager's interface, signal bars in Plasma, IPv6-only carriers handled (IPv4 through CLAT) |
| SMS | Partly | Code done and checked on D-Bus, not yet tested with a real message |
| Phone calls | Partly | Dial, answer and hang up work on D-Bus; the in-call voice path is mapped but not built |
| Sensors | Works | Rotation, light and proximity through Samsung's sensor hub |
| Double tap to wake | Works | |
| Deep sleep | Works | Sleeps with the screen off, wakes on power key, double tap, Wi-Fi or alarms |
| Back and front cameras | Works | In the Camera app (GNOME Snapshot), 30 fps, autofocus, about 70 ms from the sensor to the app |
| Desktop apps | Works | VS Code, Chromium, Brave, Firefox, Podman Desktop; wide apps are scaled to fit the phone |
| Mouse mode | Works | Turns the screen into a touchpad and zooms out, for big desktop apps |
| Containers | Works | Podman (rootful only: the kernel has no user namespaces) |
| Fingerprint, NFC, GPS, video decoding hardware, NPU | Not done | |
| Vibration, USB-C video out | Not tried | |

## What you need

- A Galaxy S24 **SM-S921B** (the Exynos model, not the Snapdragon S24 or the S24 FE)
- Stock firmware **S921BXXU6BYE7** (Android 15). Other versions are untested, and several values here are tied to this one
- OEM unlocking available in Developer options. **Do not install newer stock firmware first**: newer releases can remove the option, and the bootloader cannot be downgraded
- A PC with Docker or Linux, a USB cable

## Where to start

1. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) to see how the pieces fit
2. Follow [docs/INSTALL.md](docs/INSTALL.md)
3. Before changing anything, read [docs/LESSONS.md](docs/LESSONS.md): it lists the things that crash or brick this phone
4. Per-part details are in [docs/hardware/](docs/hardware/)
5. To work on the phone (or to pick this up again later), see [docs/DEVELOPING.md](docs/DEVELOPING.md)

## What's in here

| Folder | Contents |
|---|---|
| [stage0/](stage0/) | The `init_boot` ramdisk: `init` script, device rules, build script |
| [pmaports/](pmaports/) | postmarketOS device and kernel packages for `pmbootstrap` |
| [rootfs/](rootfs/) | Every file this port adds to the phone, at its install path (services, configs, scripts) |
| [src/](src/) | Source code of the helper programs (camera, modem, Bluetooth, graphics, audio, desktop) |
| [scripts/](scripts/) | Build and install helpers that run on the phone |
| [pc-tools/](pc-tools/) | Small Python tools to run commands and copy files from a PC |
| [tests/](tests/) | Test and diagnosis scripts used during the port |
| [docs/](docs/) | Install guide, architecture, lessons learned, per-part notes |

## What's not in here

Samsung's own files: firmware, kernel modules, the camera driver library, stock images. They are not ours to share. The guide shows how to take them from your own phone, and the camera patch scripts change your own copy of Samsung's camera library.

## License

Our code and docs are MIT licensed, see [LICENSE](LICENSE). A few parts follow the license of the project they patch, see [THIRD-PARTY.md](THIRD-PARTY.md).
