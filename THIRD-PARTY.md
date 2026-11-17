# Third-party code and data

Everything in this repository is MIT licensed (see [LICENSE](LICENSE)) except these parts, which follow the license of the project they come from or change:

| Path | Origin | License |
|---|---|---|
| `src/bluetooth/s24bt/` | Build glue for Linux kernel sources (Bluetooth core, RFCOMM, hci_uart, btqca from Samsung's kernel tree). The module built from it is a derivative of the kernel | GPL-2.0-only |
| `src/audio/pa-whole-periods.patch` | A patch to PulseAudio | LGPL-2.1-or-later |
| `src/gpu/patches/` | Patches to Mesa (radv-xclipse) | MIT |
| `rootfs/etc/environment` | postmarketOS's file with our lines appended | GPL-3.0-or-later (postmarketOS) |
| `rootfs/usr/local/share/applications/gnome-system-monitor-kde.desktop` | GNOME System Monitor's desktop file (with its translations), Exec line changed | GPL-2.0-or-later |
| `pmaports/` | Package recipes in postmarketOS's format | MIT, as in the APKBUILD |

## What is not included

No Samsung or Qualcomm files are included: no firmware, kernel modules, vendor libraries or stock images. The install guide takes them from your own phone.

- `src/camera/patch_hal11.py` changes your own copy of Samsung's camera library; it contains instruction bytes it writes and checks, no Samsung code
- `rootfs/usr/local/share/s24/s24-speaker.amixer` and `s24-mic.amixer` are mixer settings (control names and values) generated from the phone's `mixer_paths.xml`
- AIDL method numbers and struct layouts in `src/` come from AOSP's public interface definitions and from studying the phone's own binaries
