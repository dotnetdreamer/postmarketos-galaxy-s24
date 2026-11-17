#!/bin/sh
# Galaxy S24 postmarketOS: a minimal Android environment for the stock modem daemons (cbd, rild),
# built from the phone's own read-only Android images (Claude, 2026-11-14). Run as root from the
# stage-0 shell (sees /newroot and the real /dev). Idempotent.
# Layout: /newroot/android = system image (system-as-root) with vendor, odm, product, *_dlkm,
# the runtime APEX (bionic) and EFS mounted at the paths the stock init uses.
set -u
A=/newroot/android
SUPER=/dev/sda31
LOSETUP="chroot /newroot /sbin/losetup"

part() { # name start_sector size_sectors mountpoint
	if mountpoint -q "$4"; then return 0; fi
	dev=$($LOSETUP -f --show -r -o $(($2 * 512)) --sizelimit $(($3 * 512)) $SUPER) || return 1
	mount -t erofs -o ro "$dev" "$4" && echo "mounted $1 ($dev) on $4"
}

mkdir -p $A
part system 2048 14854808 $A || exit 1
part vendor 17440768 3321328 $A/vendor
part odm 14858240 1552 $A/odm
part product 14860288 2554248 $A/product
[ -d $A/system_dlkm ] && part system_dlkm 17416192 24248 $A/system_dlkm
[ -d $A/vendor_dlkm ] && part vendor_dlkm 20762624 19992 $A/vendor_dlkm

for d in dev proc sys; do
	mountpoint -q $A/$d || mount -o rbind /$d $A/$d
done
for d in apex linkerconfig mnt data tmp; do
	[ -d $A/$d ] && { mountpoint -q $A/$d || mount -t tmpfs -o mode=0755 tmpfs $A/$d; }
done

# APEX packages the modem daemons need, unpacked once and loop-mounted: the runtime (bionic:
# linker64, libc), i18n (ICU, for libsqlite) and the VNDK v34 libraries the vendor image was built
# against. Each is mounted under /data/s24apex/NAME: second-stage init mounts its own empty tmpfs
# over /apex (MountExtraFilesystems) and init.s24.rc binds these back in early-init, as apexd would.
# The same mounts are bound on this side's /apex too, for tools run with chroot from stage 0.
apex() { # file name
	img=/newroot/var/lib/android-apex/$2.img
	mnt=$A/data/s24apex/$2
	mountpoint -q $mnt && return 0
	mkdir -p /newroot/var/lib/android-apex $mnt $A/apex/$2
	[ -s $img ] || chroot /newroot /usr/bin/unzip -p "${1#/newroot}" apex_payload.img > $img || return 1
	dev=$($LOSETUP -f --show -r ${img#/newroot}) || return 1
	mount -o ro "$dev" $mnt || return 1
	mountpoint -q $A/apex/$2 || mount -o bind $mnt $A/apex/$2
	echo "mounted apex $2"
}
apex $A/system/apex/com.android.runtime.apex com.android.runtime
apex $A/system/apex/com.android.i18n.apex com.android.i18n
apex $A/system/system_ext/apex/com.android.vndk.v34.apex com.android.vndk.v34

# partitions by name, as ueventd makes them; /dev/mbin0 as init.baseband.rc does
mkdir -p /dev/block/by-name
for b in /sys/class/block/sd*; do
	n=$(sed -n 's/^PARTNAME=//p' "$b/uevent")
	[ -n "$n" ] && ln -sf "/dev/$(basename "$b")" "/dev/block/by-name/$n"
done
ln -sf /dev/block/by-name/radio /dev/mbin0

# EFS (modem NV data, IMEI; backed up on the PC) and the modem's own EFS, read-write as on Android
mkdir -p $A/mnt/vendor/efs $A/mnt/vendor/cpefs $A/data/vendor/log/cbd $A/data/vendor/radio
mountpoint -q $A/mnt/vendor/efs || mount -t ext4 -o nosuid,nodev,noatime /dev/block/by-name/efs $A/mnt/vendor/efs
mountpoint -q $A/mnt/vendor/cpefs || mount -t ext4 -o nosuid,nodev,noatime /dev/block/by-name/cpefs $A/mnt/vendor/cpefs
echo "android env ready"
