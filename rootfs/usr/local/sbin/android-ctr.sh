#!/bin/sh
# Galaxy S24 postmarketOS: start/stop the Android modem container (Claude, 2026-11-14).
# Runs inside the postmarketOS root (chroot /newroot from stage 0, or as root on the phone).
#   android-ctr.sh start | stop | status
# Android's init runs chrooted into /android in a private mount namespace (see start); it reads /data/s24ctr/init.s24.rc only, because the container's /proc/cmdline
# and /proc/bootconfig (bind-mounted fakes) carry androidboot.init_rc.
A=/android
W=$A/data/s24ctr
LOG=/var/log/android-ctr.log

case "${1:-start}" in
start)
	sh /usr/local/sbin/android-env.sh-chroot 2>/dev/null
	mountpoint -q $A || { echo "android images not mounted (run android-env.sh from stage 0)"; exit 1; }
	mkdir -p /dev/binderfs
	mountpoint -q /dev/binderfs || mount -t binder binder /dev/binderfs || exit 1
	for b in binder hwbinder vndbinder; do ln -sf binderfs/$b /dev/$b; chmod 0666 /dev/binderfs/$b; done
	mkdir -p $W
	cp /usr/local/share/s24/init.s24.rc $W/init.s24.rc
	# androidboot.hardware=microdroid: Android 15 init always makes a second mount namespace and
	# setns()es back unless recovery/microdroid, and setns resets the root to the namespace root (the
	# stage-0 ramdisk, not this chroot) -> "SetupMountNamespaces failed". Microdroid: one namespace.
	{ tr -d '\n' < /proc/cmdline | sed 's/androidboot.hardware=[^ ]*//'; echo " androidboot.init_rc=/data/s24ctr/init.s24.rc androidboot.hardware=microdroid"; } > $W/cmdline
	{ grep -v '^androidboot.hardware ' /proc/bootconfig; echo 'androidboot.init_rc = "/data/s24ctr/init.s24.rc"'; echo 'androidboot.hardware = "microdroid"'; } > $W/bootconfig
	pkill -x cbd 2>/dev/null
	# The Samsung kernel has no PID namespaces (CONFIG_PID_NS off): init runs in the host PID
	# namespace, in a private mount namespace, without CAP_SYS_BOOT, so its fatal-error path
	# ("reboot into bootloader") can only make it exit. ueventd is not run: mdev made /dev.
	touch /dev/.coldboot_done
	# init's property area files must carry an SELinux label (bionic fsetxattr()s it and init
	# aborts otherwise): tmpfs here cannot store security.* xattrs, ext4 can, so the area lives
	# on the root filesystem, bound over /dev/__properties__ inside the container only
	P=/var/lib/android-props
	rm -rf $P; mkdir -p $P /dev/__properties__ /dev/socket
	# ro.apex.updatable=false: with updatable APEXes init makes a second mount namespace and
	# setns()es back, and setns resets the root to the namespace root (the stage-0 ramdisk here,
	# not this chroot), so init loses the Android root and aborts (SetupMountNamespaces)
	sed 's/^ro.apex.updatable=true/ro.apex.updatable=false/' $A/system/build.prop > $W/system-build.prop
	unshare --mount --propagation private sh -c "
		mount --bind $W/cmdline $A/proc/cmdline
		mount --bind $W/bootconfig $A/proc/bootconfig
		mount --bind $P $A/dev/__properties__
		mount --bind $W/system-build.prop $A/system/build.prop
		exec setpriv --bounding-set -sys_boot chroot $A /system/bin/init second_stage" >> $LOG 2>&1 &
	echo "android container started (log $LOG)"
	;;
stop)
	P=$(pgrep -f "/system/bin/init second_stage" | head -n1)
	[ -n "$P" ] && kill -9 "$P" && echo "stopped init $P (its PID namespace dies with it)"
	;;
status)
	pgrep -a -f "init second_stage|servicemanager|cbd|rild|logd" | grep -v pgrep
	;;
esac
