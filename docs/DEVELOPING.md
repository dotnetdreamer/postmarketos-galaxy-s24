# Working on the phone

Notes for anyone continuing this port, human or AI agent. Read [LESSONS.md](LESSONS.md) first; its "Never do this" table lists the actions that crash or damage this phone.

## Coming back to it later: a checklist

1. Is the phone still on S921BXXU6BYE7? (`getprop` in the container, or the boot log.) If it was updated, offsets in `android-env.sh` and the camera patch no longer apply
2. Can you reach it? USB cable: `telnet 172.16.42.1` (stage 0, root, no password). Wi-Fi: SSH as `user`
3. Which services run: `rc-status default`; logs in `/var/log/s24-*.log`
4. Compare what's installed with this repo before editing on the phone: the phone may have changes that never made it here (`md5sum` both sides)

## Getting in

| Route | When | How |
|---|---|---|
| USB cable, stage 0 | Always (also when postmarketOS is broken) | `telnet 172.16.42.1`, or `pc-tools/s24sh.py run "CMD"`. You're in the boot ramdisk: postmarketOS is at `/newroot` (`chroot /newroot ...`) |
| USB cable or Wi-Fi, SSH | postmarketOS up | `ssh user@172.16.42.1` or the Wi-Fi address; `pc-tools/s24ssh.py` (settings in `pc-tools/s24common.py`). Root: `sudo` |
| USB serial | The USB network didn't come up | The gadget's ACM port (a COM port on Windows); `pc-tools/s24acm.py` talks to it over libusb |

Paths differ between the two shells: from stage 0, the container's root is `/newroot/android`; from postmarketOS it's `/android`.

dropbear (postmarketOS's SSH server here) has no SFTP: `pc-tools/s24put.py` and `s24pull.py` move files through `cat` on an exec channel.

## Habits that avoid trouble

- **Hold a wake lock in every script** (`echo myname > /sys/power/wake_lock`, and `wake_unlock` at the end). With the screen off and no cable, the phone otherwise suspends between commands and the SSH session stalls
- `unset LD_PRELOAD LD_LIBRARY_PATH` at the top of root scripts: `sudo` keeps the session's graphics shims, which break the container's bionic binaries
- Wrap every call into the container in `timeout` (asked too early in boot, Android's `service` tool spun a CPU core forever)
- Anchor `pgrep -f`/`pkill -f` patterns (`'^/usr/local/sbin/s24-cam serve'`): an unanchored one matches your own `sudo sh -c '...'` command line
- Long commands: start them with `setsid` in the background, write output to a file, poll for a "done" line. The USB link drops when the modem changes power
- Build into a temp file and `mv` into place; never overwrite a library a running program has mapped
- Never leave a test GUI app open on the user's screen; test invisibly (below) when you can

## Seeing what happens

| What | Where |
|---|---|
| Kernel | `dmesg` (the ring holds only about a minute: the sensor hub is chatty), `/proc/last_kmsg` (previous boot), `/proc/reset_reason` |
| Android container | `chroot /android /system/bin/logcat -b all -d` (UTC timestamps), crash stacks in the `crash` buffer and as `F DEBUG` lines |
| Services | `/var/log/s24-*.log` |
| Audio DSP | `/proc/abox/log-00` (blocking: `cat` in the background) |
| Panel contents | `fbgrab /dev/dri/card1 <plane-id> out.ppm` (root) |
| What a compositor drew | `grim` as the session user |
| A thread's state | `/proc/PID/task/TID/{comm,wchan,stat}` (no debuggerd here) |
| Kernel functions | kprobes in `/sys/kernel/tracing` (`KPROBE_EVENTS` is on, the function tracer is not) |

## Testing without touching the user's screen

- Headless compositor: `phoc` with `WLR_BACKENDS=headless` inside `dbus-run-session`, or `kwin_wayland --virtual --socket NAME --width 1080 --height 2340` on a private `dbus-daemon`; then `grim` or KWin's `ScreenShot2`
- Camera: a PipeWire consumer (`pwlat`, `gst-launch-1.0 pipewiresrc target-object=<serial>`) instead of the Camera app; frames can be measured on the phone without saving any image
- Sound: the muted-amp check in `tests/audio/silent-tone-check.sh`
- Input: write `input_event`s to `/dev/input/eventN` (`tests/sensors/injectkey.py`), or a uinput stand-in device; **never inject taps into a phone someone may be holding or that is locked**

## Reverse engineering Samsung's binaries

The vendor libraries came from the phone's own vendor image. What worked:

- `llvm-objdump -d` the library once to a text file; find who uses a log string by pairing `adrp` + `add` instruction targets with the string's address (a short Python script); log strings name the function and line
- Binder interfaces: AOSP's frozen `aidl_api` files give the method order (code = index + 1); check the interface hash with `src/modem/aidlver.c` first
- Patching aarch64 code here: the libraries use BTI and pointer authentication. A patched function entry must keep its `bti c` / `paciasp` landing pad, a returning patch needs `autiasp; ret`, and a `blr` you replace must stay a call (`br` to a call-only landing pad faults)
- Free space for new code: zero padding at the end of the executable segment (the camera patch puts a 76-byte constructor there and points `init_array[0]` at it)
- Struct layouts of a closed driver (the gralloc descriptor) came from disassembling the function that builds them

## The build container

An `alpine:edge` container with `pmbootstrap`, `android-tools`, `heimdall`, `clang`/`lld`. It was also used to build Samsung's kernel tree for the Bluetooth module ([hardware/bluetooth.md](hardware/bluetooth.md)) and Android (bionic) test binaries: `clang --target=aarch64-linux-android -nostdlib -Wl,--dynamic-linker=/system/bin/linker64` against the phone's own `libc.so`/`libdl.so`, with a hand-written `_start` that calls `__libc_init`.

Everything else in `src/` builds on the phone itself with `gcc` (`scripts/build-on-phone.sh`).

## Tests

`tests/` holds the scripts used during the port, grouped by area. They were written for this phone and its layout, run as root on the phone, and some run from the stage-0 shell (they use `/newroot` paths). Read one before running it: several stop a service, run their own copy and start the service again at the end.
