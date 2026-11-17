#!/bin/sh
unset LD_PRELOAD LD_LIBRARY_PATH
sleep 6
echo "idle: wake locks now [$(cat /sys/power/wake_lock)]; $(grep -E 'camera closed' /var/log/s24-camera.log | tail -1)"
RT="XDG_RUNTIME_DIR=/run/user/10000"
echo "back:";  su user -c "$RT /home/user/pwlat samsung_rear 4" 2>&1 | grep -E 'first frame|frames,'
echo "  wake locks while streaming were held; now [$(cat /sys/power/wake_lock)]"
echo "front:"; su user -c "$RT /home/user/pwlat samsung_front 4" 2>&1 | grep -E 'first frame|frames,'
grep -E 'switch|error|exit|died' /var/log/s24-camera.log | tail -3
rc-update add s24-camera default 2>&1 | tail -1
rc-update show | grep s24-camera
