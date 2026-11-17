#!/bin/sh
# A/B colour test through the serve path (a second camera client crashes the provider when it
# exits): stop the service, run its setup with a test serve whose request settings alternate per
# camera open (ours, template, ours, ...), stream each camera from a PipeWire consumer, compare.
# Nothing is saved: frames are only measured.
unset LD_PRELOAD LD_LIBRARY_PATH
echo s24-colortest > /sys/power/wake_lock
cd /home/user && gcc -O2 -Wall -DS24CAM_PIPEWIRE -o /root/s24-cam.color s24-cam.c $(pkg-config --cflags --libs libgbinder glib-2.0 libpipewire-0.3) 2>&1 | grep -E 'error|warning'
[ -x /root/s24-cam.color ] || exit 1
rc-service s24-camera stop >/dev/null 2>&1
sleep 2
sed -e 's#^exec taskset $FAST /usr/local/sbin/s24-cam serve#exec env S24CAM_TEMPLATE=alt S24CAM_COLOR=1 S24CAM_STATS=1 S24CAM_DUMPTPL=1 S24CAM_VTAGS=1 S24CAM_SCREEN_PAUSE=0 taskset $FAST /root/s24-cam.color serve#' /usr/local/sbin/s24-camera-run > /root/abtest-run.sh
[ "$(grep -c '/root/s24-cam.color' /root/abtest-run.sh)" = 1 ] || { echo "sed failed"; rc-service s24-camera start; exit 1; }
setsid sh /root/abtest-run.sh > /tmp/abtest.log 2>&1 < /dev/null &
for i in $(seq 1 120); do grep -q 'a camera runs only while' /tmp/abtest.log && break; sleep 1; done
grep -q 'a camera runs only while' /tmp/abtest.log && echo "$(date +%T) test serve up" || { echo "test serve did not come up"; tail -20 /tmp/abtest.log; }
sleep 7
look() { su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat $1 6" > /dev/null 2>&1; sleep 7; }
look samsung_front; look samsung_front; look samsung_front
look samsung_rear; look samsung_rear
look samsung_front; look samsung_front
echo "$(date +%T) done; provider $(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64' | head -1)"
pkill -TERM -f '^/root/s24-cam.color serve' ; sleep 4
grep -E '^\[color\]|^A/B|\) open$|request settings|rror|died' /tmp/abtest.log | cut -c1-240 > /tmp/abtest-summary.txt
rc-service s24-camera start >/dev/null 2>&1
for i in $(seq 1 120); do tail -n 5 /var/log/s24-camera.log | grep -q 'a camera runs only while' && break; sleep 1; done
echo "$(date +%T) service back: $(pgrep -f '^/usr/local/sbin/s24-cam serve' | head -1)"
echo s24-colortest > /sys/power/wake_unlock
