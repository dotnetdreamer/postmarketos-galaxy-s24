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
#   4. installs the quick setting, the desktop files and the login autostart, and puts the quick
#      setting next to Mouse Mode in the user's drawer
#   5. the phone's screen as a phone beside the browser's: Plasma Mobile's window script, decided per
#      screen (rootfs/usr/local/share/kwin/scripts/convergentwindows), swapped into a running KWin
#   6. the Wi-Fi address: the firewall rule for port 7443 (Wi-Fi, USB and wired networks only)
#   7. on through a restart: greetd starts the desktop by itself at boot (locked), but only while
#      Remote Desktop is to stay on (/usr/local/libexec/s24-remote/autologin); otherwise the login
#      screen as before. S24_REMOTE_NO_AUTOLOGIN=1 leaves greetd alone
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

# 4. the quick setting, desktop files, login autostart; 5. the window script; 6. the firewall rule;
# 7. the automatic login's pieces
for f in usr/share/plasma/quicksettings/org.s24.quicksetting.remote/metadata.json \
	usr/share/plasma/quicksettings/org.s24.quicksetting.remote/contents/ui/main.qml \
	usr/local/share/applications/org.s24.remote.desktop \
	usr/local/share/applications/org.s24.remote-helper.desktop \
	etc/xdg/autostart/s24-remote-cleanup.desktop \
	usr/local/share/kwin/scripts/convergentwindows/metadata.json \
	usr/local/share/kwin/scripts/convergentwindows/contents/ui/main.qml \
	etc/nftables.d/50_s24remote.nft \
	usr/local/share/s24-remote/autologin-xdg/kscreenlockerrc; do
	put "rootfs/$f" "/$f" 0644
done
put rootfs/usr/local/libexec/s24-remote/autologin /usr/local/libexec/s24-remote/autologin 0755

if [ -z "$D" ]; then
	# 6. the firewall rule, now. The Galaxy S24's kernel has no nftables (no firewall at all): there
	# s24-remote's Wi-Fi door takes only local-network connections by itself (src/remote/src/lan.rs)
	if command -v nft >/dev/null && nft list tables >/dev/null 2>&1; then
		if rc-service nftables reload >/dev/null 2>&1 || nft -f /etc/nftables.nft >/dev/null 2>&1; then
			echo "the firewall lets Wi-Fi and USB networks reach port 7443"
		else
			echo "note: the firewall wasn't reloaded" >&2
		fi
	else
		echo "this kernel has no nftables (no firewall): the Wi-Fi door takes only local-network connections by itself"
	fi
fi

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
# 7. greetd: the desktop by itself at boot while Remote Desktop is to stay on (autologin decides)
greetd_conf=$D/etc/phrog/greetd-config.toml
if [ -n "$user" ] && [ "$user" != root ] && [ -z "${S24_REMOTE_NO_AUTOLOGIN:-}" ] && [ -f "$greetd_conf" ]; then
	if grep -q '^\[initial_session\]' "$greetd_conf"; then
		echo "greetd has an initial_session already: left as it is ($greetd_conf)"
	else
		cp "$greetd_conf" "$greetd_conf.s24new"
		printf '\n# Remote Desktop (s24-remote): the desktop at boot without the login screen, locked, only while\n# Remote Desktop is to stay on; otherwise autologin ends at once and the login screen shows.\n[initial_session]\ncommand = "/usr/local/libexec/s24-remote/autologin"\nuser = "%s"\n' "$user" >>"$greetd_conf.s24new"
		mv -f "$greetd_conf.s24new" "$greetd_conf"
		echo "greetd: the desktop starts by itself at boot (locked) while Remote Desktop stays on"
	fi
fi

# 5. the window script into the running session, if there is one (else from the next login)
if [ -z "$D" ] && [ -n "$user" ] && [ "$user" != root ]; then
	for p in $(pgrep -x plasmashell); do
		# the desktop session's own (in a logind session's cgroup), not a test copy
		grep -q '^0::/c[0-9]*$' /proc/$p/cgroup 2>/dev/null || continue
		[ "$(stat -c %U /proc/$p)" = "$user" ] || continue
		bus=$(tr '\0' '\n' </proc/$p/environ | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
		[ -n "$bus" ] || continue
		su "$user" -s /bin/sh -c "export DBUS_SESSION_BUS_ADDRESS='$bus'
			qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript convergentwindows >/dev/null 2>&1
			qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure >/dev/null 2>&1" &&
			echo "the window script is in the running session"
		break
	done
fi
echo "done: the quick setting appears after the next login"
