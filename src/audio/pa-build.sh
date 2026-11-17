#!/bin/sh
# Galaxy S24 postmarketOS (Claude, 2026-11-15): build PulseAudio 17.0's libalsa-util.so with
# pa-whole-periods.patch, matching the installed pulseaudio-alsa 17.0-r9 (Alpine's patches applied
# by pa-fetch.sh). Only the ALSA helper library is built; build headers come in as one virtual
# package (.s24-pa-build) that `apk del .s24-pa-build` removes again. Log: /root/pa-build/build.log
B=/root/pa-build
exec > $B/build.log 2>&1
set -e
echo "== $(date '+%T') build dependencies"
# (apk exits non-zero for this system's old, unrelated postmarketos-mkinitfs error: check instead)
apk add --virtual .s24-pa-build alsa-lib-dev dbus-dev eudev-dev libsndfile-dev gettext-dev \
	libcap-dev libtool m4 tdb-dev || true
apk info -e .s24-pa-build alsa-lib-dev eudev-dev libsndfile-dev tdb-dev
cd $B/pulseaudio-17.0
rm -rf output
echo "== $(date '+%T') configure"
# the features that reach libalsa-util (alsa, dbus = device reservation, udev, gettext) as in
# Alpine's build; everything else off (other modules, not built here)
meson setup output --prefix=/usr --libdir=lib --buildtype=plain -Dc_args=-O2 \
	-Dalsa=enabled -Ddbus=enabled -Dudev=enabled -Ddatabase=tdb \
	-Dman=false -Dtests=false -Ddoxygen=false -Dgcov=false -Dsystemd=disabled \
	-Dx11=disabled -Dgtk=disabled -Dglib=disabled -Dgsettings=disabled -Davahi=disabled \
	-Dbluez5=disabled -Djack=disabled -Dlirc=disabled -Dopenssl=disabled -Dorc=disabled \
	-Dsoxr=disabled -Dspeex=disabled -Dfftw=disabled -Dgstreamer=disabled \
	-Dwebrtc-aec=disabled -Dasyncns=disabled -Dsamplerate=disabled -Dhal-compat=false \
	-Dvalgrind=disabled -Dsystem_user=pulse -Dsystem_group=pulse
T=$(ninja -C output -t targets all | grep -o -E '^[^:]*libalsa-util\.so' | head -1)
echo "== $(date '+%T') building $T"
ninja -C output -j8 "$T"
ls -la output/$T
echo "== exported symbols, new vs installed:"
nm -D --defined-only output/$T | awk '{print $NF}' | sort > /tmp/sym.new
nm -D --defined-only /usr/lib/pulseaudio/modules/libalsa-util.so | awk '{print $NF}' | sort > /tmp/sym.old
echo "new $(wc -l < /tmp/sym.new) installed $(wc -l < /tmp/sym.old) differ: $(diff /tmp/sym.old /tmp/sym.new | grep -c '^[<>]')"
diff /tmp/sym.old /tmp/sym.new | head -10 || true
echo "== $(date '+%T') BUILD DONE"
