#!/bin/sh
# boot rehearsal: rebuild, stop s24-mmd and the whole Android container, start s24-mmd from scratch
exec > /tmp/coldstart-mmd.out 2>&1
S=/usr/local/share/s24
cd $S && gcc -O2 -Wall -o /tmp/s24-mmd s24-mmd.c $(pkg-config --cflags --libs mm-glib libgbinder gio-2.0 gio-unix-2.0) || exit 1
echo "== $(date '+%T') stop"
rc-service s24-mmd stop
r=$(for p in /proc/[0-9]*; do [ -d "$p/root/newroot/usr/local/sbin" ] && { echo "$p/root"; break; }; done)
chroot "$r" sh /newroot/usr/local/sbin/android-ctr0.sh stop
for m in /android/mnt/vendor/efs /android/mnt/vendor/cpefs; do mountpoint -q $m && umount $m; done
sleep 2
echo "container: $(pgrep -f 'init second_stage' >/dev/null && echo STILL RUNNING || echo stopped)"
install -m 755 /tmp/s24-mmd /usr/local/sbin/s24-mmd
echo "== $(date '+%T') start (the wrapper starts the container)"
rc-service s24-mmd start
i=0
while [ $i -lt 150 ] && ! nmcli -t -f DEVICE,STATE device | grep -q '^s24-ril:connected$'; do sleep 1; i=$((i + 1)); done
echo "== $(date '+%T') connected after ${i}s"
echo "== DONE"
