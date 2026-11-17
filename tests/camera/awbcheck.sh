#!/bin/sh
# why is the front camera bluish? does the HAL get the camera modules' factory white-balance data?
unset LD_PRELOAD LD_LIBRARY_PATH
echo s24-colortest > /sys/power/wake_lock
uptime; echo "panel $(cat /sys/class/drm/card1-DSI-1/enabled)"
echo "== stuck readers from the last check:"; ps -o stat,pid,args | awk '$1 ~ /^D/ && /head|sh -c/' | head
IP=$(pgrep -f 'init second_stage' | head -1)
echo "== /dev/caminfo: host: $(ls -l /dev/caminfo 2>&1 | cut -c1-80)"
echo "   container: $(nsenter -t $IP -m -- ls -l /dev/caminfo 2>&1 | cut -c1-80)"
echo "== HAL log (awb / caminfo / eeprom / rom / cal):"
timeout 15 chroot /android /system/bin/logcat -b all -d 2>/dev/null | grep -i -E 'awb|caminfo|eeprom|rom data|loadRom|getCalData|cal data|otp|icc' | sed -E 's/^[0-9-]+ //' | cut -c1-200 | sort -k4 | uniq -c -f3 | sort -rn | head -40
echo "== log buffer covers:"; timeout 15 chroot /android /system/bin/logcat -b main -d 2>/dev/null | head -2 | cut -c1-60
echo s24-colortest > /sys/power/wake_unlock
