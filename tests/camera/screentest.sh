#!/bin/sh
# install the screen-pause build, then: app connects with the screen dark (camera must stay off),
# screen on (camera resumes), screen off (camera pauses, then closes and lets the phone sleep)
unset LD_PRELOAD LD_LIBRARY_PATH
echo s24-screentest > /sys/power/wake_lock
cp /root/s24-cam.pw /usr/local/sbin/s24-cam.new && mv /usr/local/sbin/s24-cam.new /usr/local/sbin/s24-cam
n0=$(grep -c 'a camera runs only while' /var/log/s24-camera.log)
rc-service s24-camera restart >/dev/null 2>&1
for i in $(seq 1 90); do [ "$(grep -c 'a camera runs only while' /var/log/s24-camera.log)" -gt "$n0" ] && break; sleep 1; done
L=$(wc -l < /var/log/s24-camera.log)
lit(){ cat /sys/class/drm/card1-DSI-1/enabled; }
echo "$(date +%T) service up; panel $(lit)"
sleep 8
su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_rear 30" > /tmp/screentest-pwlat.log 2>&1 &
sleep 6;  echo "$(date +%T) app watching for 6 s with the screen dark: $(grep -c 'first frame' /tmp/screentest-pwlat.log) frames arrived (want 0)"
python3 /home/user/powerkey.py; sleep 8; echo "$(date +%T) power key -> panel $(lit); $(grep 'first frame' /tmp/screentest-pwlat.log)"
python3 /home/user/powerkey.py; sleep 12; echo "$(date +%T) power key -> panel $(lit)"
wait
grep -E 'frames,' /tmp/screentest-pwlat.log
echo "== camera log:"; tail -n +$((L + 1)) /var/log/s24-camera.log | grep -vE '^\[stream\]|^\[ts\]' | cut -c1-110
echo "wake locks: [$(cat /sys/power/wake_lock)]"
echo s24-screentest > /sys/power/wake_unlock
