#!/bin/sh
# Build this port's C programs ON THE PHONE (postmarketOS, aarch64 musl) and install them where the
# services in rootfs/ expect them. Run as root from the repo checkout on the phone:
#   sudo sh scripts/build-on-phone.sh                 all groups
#   sudo sh scripts/build-on-phone.sh camera modem    some groups: camera modem bt gpu desktop tools
# env: DESTDIR=/some/dir  install under that prefix instead of / (a dry run)
#      S24_NO_APK=1       don't install the build packages (they are already there)
# Every file is built in a temp dir and then renamed into place, never written over: the running
# session has some of these libraries loaded, and overwriting a mapped file crashes it.
# The build packages go in as one apk "virtual" package: `apk del .s24-build` removes them again.
set -u
cd "$(dirname "$0")/.."
SRC=$PWD/src
D=${DESTDIR:-}
unset LD_PRELOAD   # the session's graphics shims must not load into gcc

if [ -z "${S24_NO_APK:-}" ]; then
	apk add --virtual .s24-build build-base pkgconf linux-headers glib-dev libgbinder-dev pipewire-dev \
		modemmanager-dev libdrm-dev wayland-dev plasma-wayland-protocols >/dev/null || exit 1
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ok=0 bad=0
# b NAME DEST-PATH compiler-args... (the output file is $T/NAME)
b() {
	n=$1 dest=$2; shift 2
	if "$@"; then
		mkdir -p "$D$(dirname "$dest")"
		cp "$T/$n" "$D$dest.s24new" && chmod 0755 "$D$dest.s24new" && mv -f "$D$dest.s24new" "$D$dest" &&
			{ echo "built $dest"; ok=$((ok + 1)); return 0; }
	fi
	echo "FAILED $n"; bad=$((bad + 1)); return 1
}
pc() { pkg-config --cflags --libs "$@"; }
want() { [ -z "$WANTED" ] || echo " $WANTED " | grep -q " $1 "; }
WANTED="$*"

if want camera; then
	b s24-cam /usr/local/sbin/s24-cam gcc -O2 -Wall -DS24CAM_PIPEWIRE -o "$T/s24-cam" "$SRC/camera/s24-cam.c" \
		$(pc libgbinder glib-2.0 libpipewire-0.3)
	b s24-camd /usr/local/sbin/s24-camd gcc -O2 -Wall -o "$T/s24-camd" "$SRC/camera/s24-camd.c" $(pc libgbinder gobject-2.0)
	b pwlat /usr/local/bin/pwlat gcc -O2 -Wall -o "$T/pwlat" "$SRC/camera/pwlat.c" $(pc libpipewire-0.3)
fi
if want modem; then
	b s24-mmd /usr/local/sbin/s24-mmd gcc -O2 -Wall -o "$T/s24-mmd" "$SRC/modem/s24-mmd.c" \
		$(pc mm-glib libgbinder gio-2.0 gio-unix-2.0)
	b s24-radio /usr/local/bin/s24-radio gcc -O2 -Wall -o "$T/s24-radio" "$SRC/modem/s24-radio.c" $(pc libgbinder glib-2.0)
	b aidlver /usr/local/bin/aidlver gcc -O2 -Wall -o "$T/aidlver" "$SRC/modem/aidlver.c" $(pc libgbinder glib-2.0)
	# loaded into the container's bionic service managers: no libc at all
	b libs24selinux.so /usr/local/share/s24/libs24selinux.so gcc -O2 -shared -fPIC -nostdlib \
		-Wl,-soname,libs24selinux.so -Wl,--hash-style=both -Wl,-z,noexecstack \
		-o "$T/libs24selinux.so" "$SRC/modem/s24selinux.c"
	[ -f "$D/etc/gbinder.conf" ] || { mkdir -p "$D/etc"; cp "$SRC/modem/gbinder.conf" "$D/etc/gbinder.conf"; }
fi
if want bt; then
	b s24-bt /usr/local/sbin/s24-bt gcc -O2 -Wall -o "$T/s24-bt" "$SRC/bluetooth/s24-bt.c"
	echo "note: the s24bt.ko kernel module is built in a kernel tree, see docs/hardware/bluetooth.md"
fi
if want gpu; then
	b libs24fbfix.so /usr/local/lib/libs24fbfix.so gcc -O2 -shared -fPIC -I/usr/include/libdrm \
		-o "$T/libs24fbfix.so" "$SRC/gpu/s24fbfix.c" -ldl
	b libs24kwin.so /usr/local/lib/libs24kwin.so gcc -O2 -shared -fPIC -I/usr/include/libdrm \
		-o "$T/libs24kwin.so" "$SRC/gpu/s24kwin.c" -ldl
	echo "note: the Vulkan driver itself comes from src/gpu/build-radv-pmos.sh (a few minutes)"
fi
if want desktop; then
	b libs24fit.so /usr/local/lib/libs24fit.so gcc -O2 -shared -fPIC -o "$T/libs24fit.so" "$SRC/desktop/libs24fit.c" \
		-lwayland-client -lm
	b s24-mouse-moded /usr/local/sbin/s24-mouse-moded gcc -O2 -Wall -o "$T/s24-mouse-moded" "$SRC/desktop/s24-mouse-moded.c"
	X=/usr/share/plasma-wayland-protocols/dpms.xml     # from the plasma-wayland-protocols package
	if wayland-scanner client-header $X "$T/dpms-client-protocol.h" && wayland-scanner private-code $X "$T/dpms-protocol.c"; then
		b s24-output-watch /usr/local/libexec/s24-output-watch gcc -O2 -Wall -I"$T" -o "$T/s24-output-watch" \
			"$SRC/desktop/s24-output-watch.c" "$T/dpms-protocol.c" $(pc wayland-client)
	else
		echo "FAILED s24-output-watch (no $X: apk add plasma-wayland-protocols)"; bad=$((bad + 1))
	fi
fi
if want tools; then
	b fbgrab /usr/local/bin/fbgrab gcc -O2 -I/usr/include/libdrm -o "$T/fbgrab" "$SRC/tools/fbgrab.c" -ldrm
fi
echo "$ok built, $bad failed${D:+ (installed under $D)}"
[ -n "${S24_NO_APK:-}" ] || echo "remove the build packages again with: apk del .s24-build"
[ $bad -eq 0 ]
