#!/bin/sh
# PulseAudio 17.0 source + Alpine's patches (pulseaudio 17.0-r9) + pa-whole-periods.patch, into
# /root/pa-build. Run as root on the phone; then pa-build.sh, then pa-install.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
B=/root/pa-build; mkdir -p $B; cd $B
command -v patch >/dev/null && patch --version 2>/dev/null | grep -q GNU || apk add patch
[ -f pulseaudio-17.0.tar.xz ] || wget -q https://www.freedesktop.org/software/pulseaudio/releases/pulseaudio-17.0.tar.xz
echo "053794d6671a3e397d849e478a80b82a63cb9d8ca296bd35b73317bb5ceb87b5  pulseaudio-17.0.tar.xz" | sha256sum -c -
for p in link-libintl.patch remove-once-test.patch volume-tests-fix.patch alsa-ucm-assert-1.patch alsa-ucm-assert-2.patch qt6.patch; do
	[ -f $p ] || wget -q "https://gitlab.alpinelinux.org/alpine/aports/-/raw/master/community/pulseaudio/$p"
done
cp "$HERE/pa-whole-periods.patch" $B/
rm -rf pulseaudio-17.0; tar xf pulseaudio-17.0.tar.xz
cd pulseaudio-17.0
for p in link-libintl.patch remove-once-test.patch volume-tests-fix.patch alsa-ucm-assert-1.patch alsa-ucm-assert-2.patch qt6.patch; do
	patch -p1 -s < ../$p && echo "applied $p"
done
# GNU patch, no fuzz: busybox patch once skipped a hunk silently and left no .rej
patch -p1 --fuzz=0 < ../pa-whole-periods.patch && echo "applied pa-whole-periods.patch"
grep -c 'pa_sink_render_full(u->sink, u->fragment_size' src/modules/alsa/alsa-sink.c
