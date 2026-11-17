# Installing postmarketOS on the Galaxy S24 (SM-S921B)

This is how the port was installed, step by step. It's a manual process: read each step before running it, and check what the phone answers. Commands marked **phone (stage 0)** run in the root shell of the boot ramdisk (telnet over the USB cable); **phone (pmOS)** runs in postmarketOS; **PC** runs on your computer or in the build container.

Before you start, read [LESSONS.md](LESSONS.md), at least the "Never do this" list.

## 0. What you need

- Galaxy S24 **SM-S921B** on stock firmware **S921BXXU6BYE7** (Android 15). Other firmware is untested; the partition offsets in `rootfs/usr/local/sbin/android-env.sh` belong to this one
- A Linux PC, or Windows with Docker Desktop (the author used Windows + an `alpine:edge` container)
- On Windows: [usbipd-win](https://github.com/dorssel/usbipd-win), to hand the phone's Download mode to the container
- About 20 GB free on the PC (the SUPER partition alone is 13 GB)

**Do not update the stock firmware**, before or after. Newer releases can remove OEM unlocking, and the bootloader cannot be downgraded.

## 1. Unlock the bootloader

1. Android Settings, About phone, Software information: tap Build number 7 times
2. Developer options: turn on **OEM unlocking**
3. Power off. Hold Volume Up + Volume Down and plug in the USB cable: the Download mode warning appears
4. Long-press Volume Up to unlock, confirm. The phone wipes itself and reboots
5. Go through Android setup once, connect to the internet, and check that Developer options still shows OEM unlocking as on (greyed out)

This trips Samsung Knox permanently (Samsung Pay, Secure Folder and similar stop working, even on stock).

## 2. The build container (PC)

```sh
docker run -d --name pmos-s24 --privileged -v /dev:/dev -v pmos-s24-work:/work -v "$PWD":/r alpine:edge sleep infinity
docker exec -it pmos-s24 sh
apk add pmbootstrap android-tools heimdall lz4 cpio git python3 e2fsprogs
```

On Windows with Git Bash, put `MSYS_NO_PATHCONV=1` in front of `docker exec` commands that contain `/paths`.

## 3. Build and flash the boot ramdisk

**PC**, from the repo root:

```sh
docker run --rm -v "$PWD":/r -w /r alpine:edge sh stage0/mk-init_boot.sh
```

This makes `stage0/out/init_boot.img`. Now put the phone in Download mode (step 1.3) and flash **only INIT_BOOT**:

```sh
# Windows only, once: usbipd bind --hardware-id 04e8:685d, then each time:
#   usbipd attach --wsl --hardware-id 04e8:685d
heimdall print-pit --no-reboot > /dev/null       # proves the link works
heimdall flash --resume --INIT_BOOT /r/stage0/out/init_boot.img
```

Heimdall traps: after a command with `--no-reboot`, the next one must use `--resume`; a fresh handshake wedges Download mode until you hold Power + Volume Down to restart. Partition names are upper case (`INIT_BOOT`, `BOOT`, `VENDOR_BOOT`).

Leave BOOT, VENDOR_BOOT and VBMETA stock. An unlocked bootloader boots this unsigned image with the stock vbmeta (you'll see the orange "not official software" warnings).

## 4. First boot: stage 0

The phone shows the Samsung logo and stays there: that's expected, there's no Linux root yet. After about 30 s a USB network appears:

- Phone `172.16.42.1`, the PC gets `172.16.42.2` by DHCP
- USB IDs `1d6b:0104` (NCM network + a serial port); Windows binds its own NCM and serial drivers
- `telnet 172.16.42.1` gives a **root shell without a password**. It only listens on the cable, never on Wi-Fi
- `pc-tools/s24sh.py run "CMD"` / `pull` / `push` do the same from Python, including file transfers

If nothing shows up, see "Boot problems" in [LESSONS.md](LESSONS.md).

## 5. Back up everything first

**Do this before writing anything.** The EFS partitions hold the phone's IMEI and radio calibration; lose them and the modem is gone for good.

Find partitions by name, **phone (stage 0)**:

```sh
for b in /sys/class/block/sda*; do echo "$(basename $b) $(sed -n 's/^PARTNAME=//p' $b/uevent)"; done
```

Then pull each one, **PC**:

```sh
python pc-tools/s24sh.py pull "dd if=/dev/sda1 bs=1M" efs.img        # efs
python pc-tools/s24sh.py pull "dd if=/dev/sda2 bs=1M" sec_efs.img
# also: cpefs (on sdd), param, up_param, steady, persistent, misc, dtbo, vbmeta, vbmeta_system,
# boot, vendor_boot, and super (13 GB, needed in step 8)
```

Check every file with `md5sum` on both sides. Keep these files private: they contain your IMEI and other device identifiers.

## 6. Build the postmarketOS image (PC, in the container)

```sh
cp -r /r/pmaports/device-samsung-e1s /r/pmaports/linux-samsung-e1s "$(pmbootstrap config aports)/device/testing/"
pmbootstrap init        # device: samsung-e1s, init: openrc, ui: phosh or plasma-mobile, pick your own user password
pmbootstrap install --split --password 'choose-your-own'
```

`linux-samsung-e1s` is a placeholder: the phone keeps running Samsung's kernel from BOOT. The result is `$(pmbootstrap config work)/chroot_native/home/pmos/rootfs/samsung-e1s-root.img` (the `-boot.img` is not used).

The author started with Phosh and later installed Plasma Mobile on the phone (`apk add plasma-mobile`); both work. [packages-installed.txt](packages-installed.txt) is the full package list of the finished phone.

## 7. Write the root file system to userdata

Check that `sda36` is `userdata` in step 5's list. Then, **PC**:

```sh
python pc-tools/s24sh.py push samsung-e1s-root.img "dd of=/dev/sda36 bs=4M"
```

**phone (stage 0)**: stop stage 0 from switching to the new root while you prepare it, then mount it and grow the file system to the whole partition:

```sh
touch /tmp/stay-stage0
mkdir -p /newroot && mount -t ext4 /dev/sda36 /newroot
for d in dev proc sys; do mount -o rbind /$d /newroot/$d; done
chroot /newroot resize2fs /dev/sda36
```

## 8. Fixes the stock postmarketOS image needs

**phone (stage 0)**, all paths inside `/newroot`:

| What | Why |
|---|---|
| `/etc/init.d/udev`: change both devtmpfs checks to `if false` | The kernel has no devtmpfs; eudev runs fine without it (mdev makes the nodes) |
| `/etc/phrog/greetd-config.toml`: `vt = "none"` | No virtual terminals in this kernel (Phosh's phrog greeter) |
| `/etc/inittab`: comment out the getty lines | No VTs, they only respawn and fail |
| `rc-update del unudhcpd.usb0`, `rc-update del usb-signaller`, `rc-update del nftables` | Stage 0 already runs the USB network; the kernel has no nf_tables |
| `adduser user video`, `adduser user audio` | No ACLs on this `/dev`: groups are the only way in for KWin and PulseAudio |

The files under `rootfs/` include the rest (NetworkManager ignoring `usb0`, the graphics environment, services).

## 9. Copy Samsung's vendor files from your own phone

Wi-Fi, sound, Bluetooth and the cameras need firmware and modules from the phone's vendor images, which live in SUPER. **PC**, in the container, with the `super.img` from step 5:

```sh
lpunpack super.img super/            # android-tools
lpdump super.img                      # prints each logical partition's extents: note them
for p in vendor vendor_dlkm system_dlkm odm; do mkdir -p x-$p; fsck.erofs --extract=x-$p super/$p.img; done
```

Then copy to the phone (for example as a tar through `s24sh.py push ... "tar -x -C /newroot"`):

- `x-vendor/firmware/` to `/newroot/vendor/firmware/` and `x-vendor/etc/wifi/` to `/newroot/vendor/etc/wifi/`
- `x-vendor_dlkm/lib/modules/*.ko` to `/newroot/lib/modules/vendor_dlkm/`, `x-system_dlkm/.../rfkill.ko` to `/newroot/lib/modules/system_dlkm/`

The modem and camera container mounts the Android images straight from SUPER instead of copying them. Edit `rootfs/usr/local/sbin/android-env.sh` so each `part NAME START SIZE` line matches your `lpdump` extents (start sector and length in 512-byte sectors) if your firmware differs from S921BXXU6BYE7.

## 10. Install this repo's files and programs

Copy the repo to the phone (for example `/home/user/postmarketos-galaxy-s24`), then **phone (pmOS)** or **phone (stage 0)** with `chroot /newroot`:

```sh
cp -a rootfs/. /                       # services, configs, scripts (look through rootfs/ first)
sh scripts/build-on-phone.sh           # builds and installs the C programs (needs internet for apk)
for s in s24-mmd s24-camera s24-bluetooth s24-sensors s24-power s24-mouse-mode s24-greeter-reaper s24-podman-api s24-podman-nat; do
	rc-update add $s default
done
```

`rootfs/home/user/` holds per-user settings (app entries, window button layout); copy them only if your user is called `user`.

Some parts need extra work that a script can't do for you yet; each is described in its page:

| Part | Extra step | Page |
|---|---|---|
| Graphics | Build the radv-xclipse Vulkan driver on the phone: `src/gpu/build-radv-pmos.sh` | [hardware/display-and-gpu.md](hardware/display-and-gpu.md) |
| Sound | Rebuild PulseAudio's ALSA module with `src/audio/pa-whole-periods.patch` | [hardware/sound.md](hardware/sound.md) |
| Bluetooth | Build `s24bt.ko` in Samsung's kernel tree; copy Samsung's `btpower.ko` | [hardware/bluetooth.md](hardware/bluetooth.md) |
| Camera | Patch your copy of Samsung's camera library with `src/camera/patch_hal11.py` | [hardware/camera.md](hardware/camera.md) |
| Modem | Nothing extra once the container works | [hardware/modem.md](hardware/modem.md) |

## 11. Boot into postmarketOS

**phone (stage 0)**: `rm /tmp/stay-stage0; reboot -f`, or from then on `chroot /newroot /sbin/reboot`. Stage 0 now mounts userdata, loads the Wi-Fi modules and hands over to postmarketOS after 10 s. The login screen appears about 40 s after power on.

Afterwards:

- Change the password if you used a weak one: postmarketOS starts an SSH server that listens on Wi-Fi
- Stage 0's telnet stays available on the USB cable as a rescue shell
- To stay in stage 0 for one boot (for repairs), connect the cable and run `touch /tmp/stay-stage0` within 10 s of the USB network appearing

## Going back to Android

Flash the stock `init_boot` from your firmware (or your step 5 backup) with heimdall, then wipe data from Android recovery. Or flash the complete stock firmware **of the same version** with Odin.
