#!/bin/sh
# Build init_boot.img: the "stage 0" ramdisk that boots postmarketOS on the Galaxy S24 (SM-S921B).
#
# Run it in Alpine Linux (any CPU; a throwaway container is fine), from the repo root:
#   docker run --rm -v "$PWD":/r -w /r alpine:edge sh stage0/mk-init_boot.sh
# It needs network access for apk. Output: stage0/out/init_boot.img (16 MiB), for INIT_BOOT.
#
# What goes in (all aarch64):
#   /init           stage0/init (busybox shell script; see docs/ARCHITECTURE.md)
#   /etc/mdev.conf  stage0/mdev.conf (device node rules; the kernel has no devtmpfs)
#   /bin/busybox    Alpine busybox-static
#   /bin/busybox-extras + /lib/ld-musl-aarch64.so.1   Alpine busybox-extras (telnetd, udhcpd) + musl
#   /bin/rebootarg, /bin/drmfill   built here from stage0/*.c (static)
#
# The ramdisk MUST be lz4 "legacy" (lz4 -l): the bootloader appends it to the stock lz4-legacy
# vendor_boot ramdisk, and the kernel cannot unpack a gzip archive behind that one (every gzip image
# ended in Samsung's Upload mode). The image gets SEANDROIDENFORCE and an AVB hash footer like
# Samsung's own; an unlocked bootloader needs no signature.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
W=$(mktemp -d)

apk add --no-cache lz4 cpio android-tools clang lld alpine-keys >/dev/null

# Alpine's aarch64 packages, only unpacked (this host may be x86_64); alpine-keys has the aarch64
# signing keys an x86_64 install lacks
mkdir -p "$W/apk" "$W/sys"
apk fetch --no-cache --arch aarch64 --keys-dir /usr/share/apk/keys/aarch64 -o "$W/apk" \
	busybox-static busybox-extras musl musl-dev linux-headers compiler-rt >/dev/null
for p in "$W"/apk/busybox-*.apk "$W"/apk/musl-1*.apk; do tar -xzf "$p" -C "$W/apk" 2>/dev/null || true; done
for p in "$W"/apk/musl-*.apk "$W"/apk/linux-headers-*.apk "$W"/apk/compiler-rt-*.apk; do tar -xzf "$p" -C "$W/sys" 2>/dev/null || true; done

# static helpers: musl's crt + libc.a + LLVM's builtins (printf's 128-bit long double helpers)
L=$W/sys/usr/lib
RT=$(find "$W/sys" -name 'libclang_rt.builtins*aarch64*.a' | head -n1)
[ -n "$RT" ] || RT=$(find "$W/sys" -path '*aarch64*' -name 'libclang_rt.builtins.a' | head -n1)
for p in rebootarg drmfill; do
	clang --target=aarch64-linux-musl -nostdinc -isystem "$W/sys/usr/include" -O2 -c -o "$W/$p.o" "$HERE/$p.c"
	ld.lld -static -o "$W/$p" "$L/crt1.o" "$L/crti.o" "$W/$p.o" "$L/libc.a" "$RT" "$L/crtn.o"
done

S=$W/root
mkdir -p "$S/bin" "$S/lib" "$S/dev" "$S/proc" "$S/sys" "$S/tmp" "$S/etc"
tr -d '\r' < "$HERE/init" > "$S/init"; chmod 0755 "$S/init"
tr -d '\r' < "$HERE/mdev.conf" > "$S/etc/mdev.conf"
install -m 0755 "$W/apk/bin/busybox.static" "$S/bin/busybox"
install -m 0755 "$W/apk/bin/busybox-extras" "$S/bin/busybox-extras"
install -m 0755 "$W/apk/lib/ld-musl-aarch64.so.1" "$S/lib/ld-musl-aarch64.so.1"
install -m 0755 "$W/rebootarg" "$S/bin/rebootarg"
install -m 0755 "$W/drmfill" "$S/bin/drmfill"
mknod -m 0600 "$S/dev/console" c 5 1
mknod -m 0666 "$S/dev/null" c 1 3

( cd "$S" && find . | sort | cpio -o -H newc -R 0:0 --quiet ) | lz4 -l -12 --favor-decSpeed -c > "$OUT/ramdisk.cpio.lz4"
O=$OUT/init_boot.img
rm -f "$O"
# os_version / patch level of the stock firmware this was made for (S921BXXU6BYE7, Android 15)
mkbootimg --header_version 4 --ramdisk "$OUT/ramdisk.cpio.lz4" --os_version 15.0.0 --os_patch_level 2025-05 --output "$O"
printf 'SEANDROIDENFORCE' >> "$O"
avbtool add_hash_footer --image "$O" --partition_size 16777216 --partition_name init_boot --algorithm NONE
rm -rf "$W"
md5sum "$O"
ls -l "$O"
