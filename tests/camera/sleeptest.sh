#!/bin/sh
# Sleep test: the phone suspends by itself (screen off, nothing holding it awake), the RTC wakes
# it ~60 s later, then: is the camera stack intact, does a camera still give frames? -> /tmp/sleeptest.log
exec > /tmp/sleeptest.log 2>&1
unset LD_PRELOAD LD_LIBRARY_PATH
RTC=/sys/class/rtc/rtc0/wakealarm
s0=$(cat /sys/power/suspend_stats/success)
PP=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1)
echo "$(date +%T) waiting for the phone to sleep (suspends so far: $s0); camera provider $PP"
n=0
while [ "$(cat /sys/power/suspend_stats/success)" = "$s0" ]; do
	echo 0 > $RTC; echo +60 > $RTC      # always an alarm 60 s ahead: it wakes the phone once asleep
	sleep 1
	n=$((n + 1)); [ $n -gt 3600 ] && { echo "no sleep within an hour; giving up"; echo 0 > $RTC; exit 1; }
done
echo s24-sleeptest > /sys/power/wake_lock          # stay awake for the checks
echo 0 > $RTC
echo "$(date +%T) awake again: suspends $s0 -> $(cat /sys/power/suspend_stats/success), failed $(cat /sys/power/suspend_stats/fail)"
dmesg | grep -iE 'PM: suspend (entry|exit)' | tail -2 | cut -c1-120
echo "camera provider now $(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1) (was $PP); s24-camd $(pgrep -f '^/usr/local/sbin/s24-camd'|head -1)"
SP=$(pgrep -f '^/usr/local/sbin/s24-cam serve'|head -1)
if [ -n "$SP" ]; then
	echo "camera app service running ($SP): back + front through PipeWire"
	su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_rear 5" 2>&1 | grep -E 'first frame|frames,'
	su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_front 5" 2>&1 | grep -E 'first frame|frames,'
else
	echo "not logged in yet: testing the camera HAL directly (30 frames, back camera)"
	S24CAM_NOPRIME=1 timeout 40 /usr/local/sbin/s24-cam shot device@1.1/internal/0 1920x1080 /tmp/sleepshot 30 2>&1 | grep -E 'template|ready|saved|closed|error|fail'
	rm -f /tmp/sleepshot.yuv /tmp/sleepshot.txt
fi
echo "temps: $(for z in /sys/class/thermal/thermal_zone*; do printf "%s=%s " "$(cat $z/type)" "$(( $(cat $z/temp) / 1000 ))"; done)"
echo "$(date +%T) done; staying awake 5 more minutes so the results can be read"
sleep 300
echo s24-sleeptest > /sys/power/wake_unlock
