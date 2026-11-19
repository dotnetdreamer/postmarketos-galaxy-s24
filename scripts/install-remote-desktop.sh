#!/bin/sh
# Install Remote Desktop (this phone's desktop in any browser, from anywhere; docs/remote-desktop.md)
# ON THE PHONE. Run as root from the repo checkout:
#   sudo sh scripts/install-remote-desktop.sh [/path/to/openandroiddex-pmos]
# It:
#   1. builds s24-remote with cargo (installs cargo first if it isn't there), unless S24_REMOTE_BIN
#      names one built already
#   2. downloads cloudflared $CF_VERSION from Cloudflare's GitHub releases and checks it against
#      the SHA-256 below before installing it
#   3. installs the screen helper, Open Android DeX's openandroiddex-pmos (version 7 or later lets
#      Low quality go below 2 Mbit/s): the path given, else the copy the Open Android DeX app put
#      in a user's home. Without it, Remote Desktop can sign browsers in but has no picture
#   4. installs the quick setting, the desktop files and the login cleanup, and puts the quick
#      setting next to Mouse Mode in the user's drawer
# env: DESTDIR=/some/dir    install under that prefix instead of / (a dry run)
#      S24_REMOTE_BIN=path  a prebuilt s24-remote (aarch64)
# Files are copied beside their target and renamed into place, never written over: a running
# s24-remote or helper would crash if its file changed under it.
set -eu
cd "$(dirname "$0")/.."
D=${DESTDIR:-}
unset LD_PRELOAD # the session's graphics shims must not load into the build
CF_VERSION=2026.10.7
CF_SHA256=3df43aedc9ff291d1735b5cb576cb97e0b157488796db2e32b28aecabb445775

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

put() { # put SRC DEST MODE
	mkdir -p "$D$(dirname "$2")"
	cp "$1" "$D$2.s24new"
	chmod "$3" "$D$2.s24new"
	mv -f "$D$2.s24new" "$D$2"
	echo "installed $2"
}

# 1. s24-remote
if [ -n "${S24_REMOTE_BIN:-}" ]; then
	BIN=$S24_REMOTE_BIN
else
	command -v cargo >/dev/null || apk add cargo
	export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-/var/tmp/s24-remote-build}"
	(cd src/remote && cargo build --release --locked)
	BIN=$CARGO_TARGET_DIR/release/s24-remote
fi
put "$BIN" /usr/local/bin/s24-remote 0755

# 2. cloudflared, checked before it is installed
if [ "$(uname -m)" != aarch64 ]; then
	echo "cloudflared: this script knows only the aarch64 build" >&2
	exit 1
fi
if "$D/usr/local/bin/cloudflared" --version 2>/dev/null | grep -q "version $CF_VERSION "; then
	echo "cloudflared $CF_VERSION is installed already"
else
	curl -fsSL -o "$T/cloudflared" \
		"https://github.com/cloudflare/cloudflared/releases/download/$CF_VERSION/cloudflared-linux-arm64"
	if ! echo "$CF_SHA256  $T/cloudflared" | sha256sum -c - >/dev/null 2>&1; then
		echo "cloudflared: the download doesn't match its published checksum; not installed" >&2
		exit 1
	fi
	put "$T/cloudflared" /usr/local/bin/cloudflared 0755
fi

# 3. the screen helper
HELPER=${1:-}
if [ -z "$HELPER" ]; then
	for h in /home/*/.local/lib/openandroiddex/openandroiddex-pmos; do
		[ -x "$h" ] && HELPER=$h
	done
fi
if [ -n "$HELPER" ] && [ -x "$HELPER" ]; then
	put "$HELPER" /usr/local/libexec/s24-remote/openandroiddex-pmos 0755
	v=$("$HELPER" version 2>/dev/null | awk '{print $2}')
	if ! [ "${v:-0}" -ge 7 ] 2>/dev/null; then
		echo "note: the helper is version ${v:-?}; from version 7 Low quality can go below 2 Mbit/s"
	fi
else
	echo "NOT FOUND: the screen helper, openandroiddex-pmos. Build it from Open Android DeX" \
		"(openandroiddex-pmos/build.sh) and run this again with its path" >&2
fi

# 4. the quick setting, desktop files, login cleanup
for f in usr/share/plasma/quicksettings/org.s24.quicksetting.remote/metadata.json \
	usr/share/plasma/quicksettings/org.s24.quicksetting.remote/contents/ui/main.qml \
	usr/local/share/applications/org.s24.remote.desktop \
	usr/local/share/applications/org.s24.remote-helper.desktop \
	etc/xdg/autostart/s24-remote-cleanup.desktop; do
	put "rootfs/$f" "/$f" 0644
done

# Next to Mouse Mode in the drawer (Plasma Mobile would put a new one last), for the user who ran sudo.
user=${SUDO_USER:-}
if [ -z "$D" ] && [ -n "$user" ] && [ "$user" != root ]; then
	su "$user" -s /bin/sh -c '
		id=org.s24.quicksetting.remote
		list=$(kreadconfig6 --file plasmamobilerc --group QuickSettings --key enabledQuickSettings)
		case ",$list," in
		*",$id,"*) exit 0 ;;
		"") exit 0 ;; # Plasma Mobile has not written its list yet: it adds the new one by itself
		esac
		case ",$list," in
		*",org.s24.quicksetting.mousemode,"*) new=$(echo "$list" | sed "s/org\.s24\.quicksetting\.mousemode/&,$id/") ;;
		*) new="$list,$id" ;;
		esac
		kwriteconfig6 --file plasmamobilerc --group QuickSettings --key enabledQuickSettings "$new"
	' && echo "put Remote Desktop in $user's drawer"
fi
echo "done: the quick setting appears after the next login"
