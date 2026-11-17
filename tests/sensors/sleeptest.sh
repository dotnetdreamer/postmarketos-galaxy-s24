#!/bin/sh
# unattended deep-sleep test: USB data off (like an unplugged data cable), let s24-powerd sleep the
# phone, RTC alarm at the end, then USB back on and the gadget re-attached
exec > /tmp/sleeptest.log 2>&1
dur=${1:-180}
end=$(( $(date +%s) + dur ))
echo "start $(date +%T) for ${dur}s; suspend success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo $end > /sys/class/rtc/rtc0/wakealarm
echo 0 > /sys/class/usb_notify/usb_control/usb_data_enabled
while [ $(date +%s) -lt $end ]; do sleep 2; done
echo "end $(date +%T); suspend success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
echo 1 > /sys/class/usb_notify/usb_control/usb_data_enabled
sleep 3
for g in /sys/kernel/config/usb_gadget/*/UDC; do [ -z "$(cat $g)" ] && ls /sys/class/udc | head -1 > $g; done
sleep 2
echo "usb back $(date +%T) udc=$(cat /sys/class/udc/*/state)"
