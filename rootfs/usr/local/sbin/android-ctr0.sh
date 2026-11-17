#!/bin/sh
# Galaxy S24 postmarketOS: start the Android modem container from the stage-0 root (Claude,
# 2026-11-14). Must run in the stage-0 shell (root = the initramfs), not inside the
# postmarketOS chroot:
#   sh /newroot/usr/local/sbin/android-ctr0.sh start [radio] [camera] | stop | status
# (start alone = radio). Each feature loads its kernel modules (stage-0 defers them at boot) and is
# switched on in the container with setprop s24.<feature> 1 (init.s24.rc).
# Why: Android 15 init always creates a second mount namespace and setns()es between them;
# setns() resets a process's root to whatever is mounted ON the namespace's "/" (LOOKUP_DOWN).
# A chroot is lost that way. So in a private mount namespace the Android root is moved onto "/"
# itself (switch_root style), then chrooted into; setns() then lands in Android again.
# Also: no PID namespaces in this kernel, so init runs in the host PID namespace without
# CAP_SYS_BOOT (its fatal-error reboot can only make it exit); its property area lives on ext4
# (tmpfs cannot store the SELinux label bionic insists on); ueventd is not run (mdev made /dev).
N=/newroot
A=$N/android
W=$A/data/s24ctr
P=$N/var/lib/android-props
LOG=$N/var/log/android-ctr.log
SETPRIV="$N/lib/ld-musl-aarch64.so.1 --library-path $N/lib:$N/usr/lib $N/usr/bin/setpriv"

load_mods() { for m in "$@"; do grep -q "^$(echo $m | tr - _) " /proc/modules || insmod /lib/modules/$m.ko || return 1; done; }

case "${1:-start}" in
start)
	shift
	features="$*"
	[ -z "$features" ] && features=radio
	for f in $features; do
		case $f in
		radio)
			load_mods cpif_page exynos-cpif-iommu_v9 cpif || exit 1
			# modem nodes as the stock ueventd.rc (spaces, not tabs: mdev.conf lines typed
			# through a telnet session lose their tabs), before the generic sd* rule
			if ! grep -q '^umts' /etc/mdev.conf; then
				{ echo 'umts_ramdump0 1001:1001 660'; echo 'umts.* 1001:1001 660'
				  echo 'ipc_loopback0 1001:1001 660'; echo 'sda20 1001:1001 440'
				  cat /etc/mdev.conf; } > /tmp/mdev.conf.new && cat /tmp/mdev.conf.new > /etc/mdev.conf
			fi ;;
		camera)
			load_mods is-cis-3k1 is-cis-gn3 is-cis-3lu is-cis-imx564 is-actuator-ak737x
			load_mods is-flash-s2mpb02-i2c smfc scaler || exit 1 ;;
		*) echo "unknown feature $f"; exit 1 ;;
		esac
	done
	sh $N/usr/local/sbin/android-env.sh || exit 1
	mkdir -p /dev/binderfs
	mountpoint -q /dev/binderfs || mount -t binder binder /dev/binderfs || exit 1
	for b in binder hwbinder vndbinder; do ln -sf binderfs/$b /dev/$b; chmod 0666 /dev/binderfs/$b; done
	# the container's /dev and postmarketOS's /dev are recursive binds made before binderfs was
	# mounted: bind the same instance into both (a separate binderfs mount is a separate binder
	# world, whose service manager would be empty)
	for d in $A/dev/binderfs $N/dev/binderfs; do
		if [ "$(stat -c %d $d 2>/dev/null)" != "$(stat -c %d /dev/binderfs)" ]; then
			mountpoint -q $d && umount -l $d
			mkdir -p $d; mount --bind /dev/binderfs $d
		fi
	done
	mkdir -p $W
	cp $N/usr/local/share/s24/init.s24.rc $W/init.s24.rc
	cp $N/usr/local/share/s24/apex-info-list.xml $W/apex-info-list.xml
	cp $N/usr/local/share/s24/libs24selinux.so $W/libs24selinux.so
	{ tr -d '\n' < /proc/cmdline; echo " androidboot.init_rc=/data/s24ctr/init.s24.rc"; } > $W/cmdline
	{ cat /proc/bootconfig; echo 'androidboot.init_rc = "/data/s24ctr/init.s24.rc"'; } > $W/bootconfig
	pkill -x cbd 2>/dev/null
	touch /dev/.coldboot_done
	# Android's own cgroup setup refuses to run unless init is PID 1 (no PID namespaces here),
	# and without its map file every service start fails (createProcessGroup). So the map is
	# written here: the host's cgroup2 tree (OpenRC's, mounted under /newroot/sys only, so bound
	# into the container below) for process groups and the freezer.
	mkdir -p /dev/cgroup_info; chmod 0711 /dev/cgroup_info; chown 1000:1000 /dev/cgroup_info
	cp $N/usr/local/share/s24/cgroup.rc /dev/cgroup_info/cgroup.rc; chmod 0644 /dev/cgroup_info/cgroup.rc
	# a property bind left from an earlier run points at the old (deleted) $P: mounting over it
	# fails with ENOENT, so drop it first
	while mountpoint -q $A/dev/__properties__; do umount -l $A/dev/__properties__; done
	rm -rf $P; mkdir -p $P /dev/__properties__ /dev/socket
	# ro.apex.updatable=false: init then keeps ONE mount namespace (bootstrap = default). With two,
	# early services land in a copy of init's namespace made before any APEX is mounted (apexd
	# fills them on Android) and fail to exec (no /apex/com.android.runtime/bin/linker64).
	sed 's/^ro.apex.updatable=true/ro.apex.updatable=false/' $A/system/build.prop > $W/system-build.prop
	unshare -m --propagation private sh -c "
		mount --bind $W/cmdline $A/proc/cmdline
		mount --bind $W/bootconfig $A/proc/bootconfig
		mount --bind $P $A/dev/__properties__
		mount --bind $N/sys/fs/cgroup $A/sys/fs/cgroup
		mount --bind $W/system-build.prop $A/system/build.prop
		cd $A || exit 1
		mount --move . / || exit 1
		exec $SETPRIV --bounding-set -sys_boot /bin/busybox chroot . /system/bin/init second_stage" >> $LOG 2>&1 &
	echo "android container started (log $LOG)"
	# tools run with chroot from here read properties through this bind; then the switches
	for i in $(seq 1 20); do [ -e $P/properties_serial ] && break; sleep 1; done
	sleep 2
	mountpoint -q $A/dev/__properties__ || mount --bind $P $A/dev/__properties__
	chroot $A /system/bin/setprop ro.cold_boot_done true 2>/dev/null
	for f in $features; do chroot $A /system/bin/setprop s24.$f 1 2>/dev/null && echo "started $f"; done
	;;
stop)
	for p in $(pgrep -f "init second_stage"); do kill -9 "$p"; done
	# and every service it started: each sits in its own uid_*/pid_* cgroup (libprocessgroup)
	cat $N/sys/fs/cgroup/uid_*/pid_*/cgroup.procs 2>/dev/null | while read p; do kill -9 "$p"; done
	sleep 1
	for d in $N/sys/fs/cgroup/uid_*/pid_* $N/sys/fs/cgroup/uid_*; do rmdir "$d" 2>/dev/null; done
	while mountpoint -q $A/dev/__properties__; do umount -l $A/dev/__properties__; done
	echo "stopped"
	;;
status)
	ps -o pid,user,args | grep -E "init second_stage|servicemanager|vndservicemanager|/cbd|rild|logd" | grep -v grep
	;;
esac
