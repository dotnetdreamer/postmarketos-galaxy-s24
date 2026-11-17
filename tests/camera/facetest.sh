#!/bin/sh
# 1) front camera: face priority off/on every 3 s in one session (frames must keep flowing),
# 2) quick camera switches and second-client shots: does the provider survive the closes now?
# Nothing is saved: frames are only measured (shot files are deleted at once).
unset LD_PRELOAD LD_LIBRARY_PATH
echo s24-colortest > /sys/power/wake_lock
cd /home/user && gcc -O2 -Wall -DS24CAM_PIPEWIRE -o /root/s24-cam.face s24-cam.c $(pkg-config --cflags --libs libgbinder glib-2.0 libpipewire-0.3) 2>&1 | grep -E 'error|warning'
gcc -O2 -Wall -o /root/s24-cam.faceshot s24-cam.c $(pkg-config --cflags --libs libgbinder glib-2.0) 2>&1 | grep -E 'error|warning'
[ -x /root/s24-cam.face ] && [ -x /root/s24-cam.faceshot ] || exit 1
rc-service s24-camera stop >/dev/null 2>&1
sleep 2
sed -e 's#^exec taskset $FAST /usr/local/sbin/s24-cam serve#exec env S24CAM_FACETEST=90 S24CAM_FACE=0 S24CAM_COLOR=1 S24CAM_STATS=1 S24CAM_SCREEN_PAUSE=0 taskset $FAST /root/s24-cam.face serve#' /usr/local/sbin/s24-camera-run > /root/facetest-run.sh
[ "$(grep -c '/root/s24-cam.face' /root/facetest-run.sh)" = 1 ] || { echo "sed failed"; rc-service s24-camera start; exit 1; }
setsid sh /root/facetest-run.sh > /tmp/facetest.log 2>&1 < /dev/null &
for i in $(seq 1 120); do grep -q 'a camera runs only while' /tmp/facetest.log && break; sleep 1; done
grep -q 'a camera runs only while' /tmp/facetest.log && echo "$(date +%T) test serve up" || { echo "test serve did not come up"; tail -20 /tmp/facetest.log; }
prov() { pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64' | head -1; }
P0=$(prov); echo "provider $P0"
sleep 6
pw() { su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat $1 $2" 2>&1 | grep -E 'frames,|first frame'; }
echo "== front camera 21 s, face priority switching every 3 s:"; pw samsung_front 21
echo "== quick switches:"; pw samsung_rear 3; pw samsung_front 3; pw samsung_rear 3; pw samsung_front 3
echo "provider now $(prov) (was $P0)"
sleep 7
echo "== second-client shots (exits after a close):"
for k in 1 2 3; do
	env S24CAM_NOPRIME=1 S24CAM_AF=4 S24CAM_FPS=30,30 S24CAM_TIMEOUT=20 timeout 30 taskset 0x3f0 /root/s24-cam.faceshot shot device@1.1/internal/1 1920x1080 /tmp/ft 45 > /tmp/ft-$k.log 2>&1
	echo "shot $k rc=$? $(grep -c 'frame .* done' /tmp/ft-$k.log) progress lines; provider $(prov)"; rm -f /tmp/ft.yuv /tmp/ft.txt; sleep 3
done
echo "provider now $(prov) (was $P0)"
pkill -TERM -f '^/root/s24-cam.face serve'; sleep 4
echo "after the test serve's exit: provider $(prov) (was $P0)"
grep -E '^\[face\]|^\[color\] phase|face detect modes|^switch:|rror|died|watchdog|exiting' /tmp/facetest.log | cut -c1-260 > /tmp/facetest-summary.txt
grep -E '^\[stream\]' /tmp/facetest.log | tail -4 | cut -c1-200 >> /tmp/facetest-summary.txt
rc-service s24-camera start >/dev/null 2>&1
for i in $(seq 1 120); do tail -n 5 /var/log/s24-camera.log | grep -q 'a camera runs only while' && break; sleep 1; done
echo "$(date +%T) service back: $(pgrep -f '^/usr/local/sbin/s24-cam serve' | head -1)"
echo s24-colortest > /sys/power/wake_unlock
