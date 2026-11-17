#!/bin/sh
# Stage-0 v5 images for the Galaxy S24 bring-up (run in the pmos-s24 container). Ramdisks are lz4
# LEGACY (lz4 -l), like Android's: the bootloader glues this ramdisk right after the stock lz4-legacy
# vendor ramdisk, and the kernel cannot find a gzip archive behind one (every gzip image failed).
#   init_boot_s0probe.img / init_boot_s0.img : stock kernel path (init_boot), /init = ramdisk/init
#   recovery_s0probe.img  / recovery_s0.img  : TWRP e1s GKI kernel + TWRP modules + the same /init
# The *probe images carry /probe: the init loads the first modules and reboots to Download mode.
set -e
B=/s24/build; T=/s24/twrp-e1s
lz4l() { lz4 -l -12 --favor-decSpeed -c; }

tree() { # $1 dir, $2 probe|normal, $3 rec|initboot
	S=$1; rm -rf $S; mkdir -p $S/bin $S/lib $S/dev $S/proc $S/sys $S/tmp
	tr -d '\r' < $B/ramdisk/init > $S/init; chmod 0755 $S/init
	install -m 0755 $B/busybox/bin/busybox.static $S/bin/busybox
	install -m 0755 $B/busybox/x/bin/busybox-extras $S/bin/busybox-extras
	install -m 0755 $B/busybox/x/lib/ld-musl-aarch64.so.1 $S/lib/ld-musl-aarch64.so.1
	install -m 0755 $B/tools-src/rebootarg $S/bin/rebootarg
	install -m 0755 $B/tools-src/drmfill $S/bin/drmfill
	mkdir -p $S/etc; tr -d '\r' < $B/ramdisk/mdev.conf > $S/etc/mdev.conf
	mknod -m 0600 $S/dev/console c 5 1
	mknod -m 0666 $S/dev/null c 1 3
	[ "$2" = probe ] && echo probe > $S/probe
	if [ "$3" = rec ]; then
		cp -a $T/recovery/root/lib/modules $S/lib/modules
		for t in $S/lib/modules/modules.*; do tr -d '\r' < "$t" > "$t.lf" && mv "$t.lf" "$t"; done
		cp -a $T/recovery/root/lib/firmware $S/lib/firmware 2>/dev/null || true
	fi
	( cd $S && find . | sort | cpio -o -H newc -R 0:0 --quiet ) | lz4l
}

for kind in probe normal; do
	sfx=$([ $kind = probe ] && echo s0probe || echo s0)
	# init_boot (stock kernel + stock vendor_boot)
	tree /tmp/s0i $kind initboot > $B/ramdisk-$sfx.cpio.lz4
	O=$B/init_boot_$sfx.img
	mkbootimg --header_version 4 --ramdisk $B/ramdisk-$sfx.cpio.lz4 --os_version 15.0.0 --os_patch_level 2025-05 --output $O
	printf 'SEANDROIDENFORCE' >> $O
	avbtool add_hash_footer --image $O --partition_size 16777216 --partition_name init_boot --algorithm NONE
	# recovery (TWRP e1s kernel)
	tree /tmp/s0r $kind rec > $B/ramdisk-rec-$sfx.cpio.lz4
	R=$B/recovery_$sfx.img
	mkbootimg --header_version 2 --kernel $T/prebuilt/Image --ramdisk $B/ramdisk-rec-$sfx.cpio.lz4 \
		--dtb $T/prebuilt/dtb --recovery_dtbo $T/prebuilt/recovery_dtbo \
		--base 0x10000000 --kernel_offset 0x00008000 --ramdisk_offset 0x00000000 --tags_offset 0x00000000 \
		--dtb_offset 0x00000000 --pagesize 2048 --board SRPWG24A005 \
		--cmdline "androidboot.selinux=permissive bootconfig buildtime_bootconfig=enable androidboot.serialconsole=0 loop.max_part=7" \
		--os_version 12.0.0 --os_patch_level 2099-12 --output $R
	printf 'SEANDROIDENFORCE' >> $R
	avbtool add_hash_footer --image $R --partition_size 100663296 --partition_name recovery --algorithm NONE
	python3 - "$R" <<'PY'
import os, sys
p = sys.argv[1]
with open(p, 'rb+') as f:
    data = f.read()
    off = data.find(b'AVB0')
    f.seek(off + 972)
    f.write(os.urandom(64))
PY
done
for f in ramdisk-s0probe.cpio.lz4 ramdisk-s0.cpio.lz4 ramdisk-rec-s0.cpio.lz4; do
	printf '%s: magic %s, %s bytes, files: %s\n' $f "$(head -c4 $B/$f | od -An -tx1 | tr -d ' ')" "$(stat -c %s $B/$f)" \
		"$(lz4 -dc $B/$f | cpio -it --quiet | grep -v '^lib/modules/.*\.ko$' | tr '\n' ' ' | cut -c1-300)"
done
ls -la $B/init_boot_s0*.img $B/recovery_s0*.img
