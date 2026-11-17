# How it fits together

## The boot chain

```
bootloader (unlocked, stock)
 ├─ BOOT         stock Samsung kernel 6.1.93-android14-11 (GKI), untouched
 ├─ VENDOR_BOOT  stock: device tree + ~290 first-stage vendor modules (lz4 legacy ramdisk)
 └─ INIT_BOOT    OURS: stage-0 ramdisk (stage0/), appended after the vendor ramdisk
                   /init = busybox shell script
                    1. loads the vendor modules in dependency order (modules.dep + modules.softdep)
                    2. feeds the hardware watchdog, runs mdev (no devtmpfs in this kernel)
                    3. USB network 172.16.42.1 + telnet (root shell, USB link only)
                    4. mounts userdata (ext4) on /newroot, binds /newroot/vendor to /vendor
                       (the kernel loads firmware relative to THIS root), loads the Wi-Fi modules
                    5. after 10 s hands PID 1 to /newroot/sbin/init with chroot
userdata (sda36, ext4) = postmarketOS root file system (OpenRC, Plasma Mobile or Phosh)
```

Stage 0 is not thrown away: `chroot` instead of `switch_root` keeps its telnet daemon, the mdev loop and the watchdog feeders running underneath postmarketOS. That makes the phone reachable over the USB cable even when the Linux side has a problem, and it is where camera sensor modules are loaded from later (`/proc/<stage-0 pid>/root/lib/modules`).

Why not a postmarketOS kernel? There is no mainline support for the Exynos 2400, and Samsung's kernel only accepts modules signed with Samsung's key, so the stock kernel plus stock modules is the only practical base. The kernel lacks devtmpfs, VT, framebuffer console, user and PID namespaces, which rules out systemd (postmarketOS runs with OpenRC here).

## Two worlds on one kernel

```
postmarketOS (musl, OpenRC)                       Android container (bionic), /android
─────────────────────────────                     ──────────────────────────────────
Plasma Mobile / Phosh, apps                       stock system/vendor/odm/product images,
PipeWire, PulseAudio, NetworkManager, BlueZ        loop-mounted read-only from the SUPER partition
                                                  its own init (only rc file: init.s24.rc)
s24-mmd  ── binder (libgbinder) ──────────────►   rild + cbd (Samsung modem daemons)
s24-cam  ── binder (libgbinder) ──────────────►   camera provider (Samsung camera HAL)
s24-camd ◄─ binder: fake sensor service ───────   (the camera HAL insists on a gyroscope)
```

Samsung's modem and camera have no Linux drivers that Linux apps can use: everything goes through Samsung's Android HAL processes. The container runs exactly those, from the phone's own read-only Android images, in a separate mount namespace with Android's init. Both sides share one binderfs instance, so Linux programs talk to the HALs directly over binder with [libgbinder](https://github.com/mer-hybris/libgbinder).

- `rootfs/usr/local/sbin/android-env.sh` mounts the images (offsets in SUPER are firmware specific), APEX payloads and EFS
- `rootfs/usr/local/sbin/android-ctr0.sh start [radio] [camera]` starts the container; features are gated by properties `s24.radio` / `s24.camera` in `init.s24.rc`
- SELinux is off; a tiny `LD_PRELOAD` shim (`src/modem/s24selinux.c`) answers `selinux_status_open` for the service managers

## Services on the phone (OpenRC)

| Service | Program | Job |
|---|---|---|
| `s24-mmd` | `s24-mmd` (C) | Owns `org.freedesktop.ModemManager1` and drives Samsung's rild: SIM, signal, data, SMS, calls. NetworkManager and Plasma see a normal modem |
| `s24-camera` | `s24-camera-run` then `s24-cam serve` (C) | Sets up the camera HAL and publishes "Back Camera" and "Front Camera" as PipeWire video sources; a camera only runs while an app watches it |
| `s24-bluetooth` | `s24-bt` (C) + `s24bt.ko` | Loads the Qualcomm firmware into the WCN7851 and attaches it as an HCI device |
| `s24-sensors` | `s24-sensord` (Python) | `net.hadess.SensorProxy` on top of Samsung's sensor hub (rotation, light, proximity) |
| `s24-power` | `s24-powerd` (Python) | Screen-off handling, double tap to wake, deep sleep with the kernel's wakeup handshake |
| `s24-vibra` | `s24-vibrad` (C) | Vibration: a virtual force-feedback device that feedbackd can drive, forwarded to the real motor (see [hardware/sensors-and-power.md](hardware/sensors-and-power.md)) |
| `s24-mouse-mode` | `s24-mouse-moded` (C) | Touchscreen to touchpad for mouse mode |
| `s24-greeter-reaper` | shell | Cleans up the login screen's leftover processes after each login |
| `s24-podman-api`, `s24-podman-nat` | shell | Podman's API socket for Podman Desktop, NAT for containers |

Boot-time fixes run from `/etc/local.d/` (`s24-perf.start`: CPU governor tuning, `s24-audio.start`: mixer routes, `s24-glibc-cache.start`, `s24-usbc.start`: USB-C monitors and docks, see [hardware/usb-c-display.md](hardware/usb-c-display.md)).

## Graphics

The display controller (`exynos-drmdpu`, `/dev/dri/card1`) and the GPU (Samsung `sgpu`, an AMD RDNA-based Xclipse 940, `/dev/dri/renderD128`) are separate devices. Rendering goes GPU → Vulkan (radv-xclipse, built on the phone) → Zink for OpenGL → linear buffers that the display controller scans out. Two small `LD_PRELOAD` shims bridge driver quirks:

- `libs24fbfix.so`: the display controller ignores a framebuffer's pitch, so frames are registered with a width that matches the GPU's 256-byte-aligned pitch
- `libs24kwin.so`: the display planes don't advertise the LINEAR modifier (they scan it out fine); KWin needs it advertised, or it finds no common format with Zink and the screen stays black

## Camera path

```
Samsung camera HAL (container)  ──binder──►  s24-cam serve  ──PipeWire──►  GNOME Snapshot / any app
  (patched: ignore SIGPIPE,                    one Video/Source node per camera,
   skip a crashing depth plugin)               rotates frames upright, timestamps them
```

Details and the long road there: [hardware/camera.md](hardware/camera.md).
