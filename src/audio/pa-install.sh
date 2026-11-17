#!/bin/sh
# Galaxy S24 postmarketOS (Claude, 2026-11-15): install the patched libalsa-util.so (whole-period
# writes, pa-whole-periods.patch) for pulseaudio-alsa 17.0-r9. Keeps the original next to the
# patched copy in /usr/local/share/s24 (the boot script s24-audio.start restores the patched one
# after a same-version reinstall and warns after a version change).
set -e
S=/usr/local/share/s24
P=/root/pa-build/pulseaudio-17.0
lib=/usr/lib/pulseaudio/modules/libalsa-util.so
new=$(find $P/output -name libalsa-util.so -type f | head -1)
[ -n "$new" ] || { echo "no built libalsa-util.so"; exit 1; }
ver=$(apk info -e -v pulseaudio-alsa | sed 's/^pulseaudio-alsa-//')
[ "$ver" = "17.0-r9" ] || { echo "pulseaudio-alsa is $ver, built for 17.0-r9: not installing"; exit 1; }
[ -f $S/libalsa-util.so.orig-$ver ] || cp $lib $S/libalsa-util.so.orig-$ver
strip -o $S/libalsa-util.so "$new"
echo "$ver" > $S/libalsa-util.version
cp $S/libalsa-util.so $lib.s24new
mv $lib.s24new $lib
# the patch as actually applied (busybox patch left the pristine file as .orig)
{ sed -n '1,/^--- a/{/^--- a/!p}' /root/pa-build/pa-whole-periods.patch
  diff -u $P/src/modules/alsa/alsa-sink.c.orig $P/src/modules/alsa/alsa-sink.c |
	sed -e "1s|^--- .*|--- a/src/modules/alsa/alsa-sink.c|" -e "2s|^+++ .*|+++ b/src/modules/alsa/alsa-sink.c|"
} > $S/pa-whole-periods.patch || true
md5sum $lib $S/libalsa-util.so $S/libalsa-util.so.orig-$ver
grep -c '^@@' $S/pa-whole-periods.patch
