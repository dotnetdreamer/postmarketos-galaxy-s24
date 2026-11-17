#!/bin/sh
# one real suspend-to-RAM, woken by the RTC alarm after $1 seconds
exec > /tmp/suspendtest.log 2>&1
secs=${1:-25}
dmesg -c > /dev/null
echo "start $(date +%T) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo none > /sys/power/pm_test
rtc=/sys/class/rtc/rtc0
echo "rtc0=$(cat $rtc/name) since_epoch=$(cat $rtc/since_epoch)"
echo 0 > $rtc/wakealarm
echo +$secs > $rtc/wakealarm
echo "wakealarm=$(cat $rtc/wakealarm)"
cnt=$(timeout 10 cat /sys/power/wakeup_count) || echo "wakeup_count read failed/blocked"
echo "wakeup_count=$cnt"
[ -n "$cnt" ] && { echo $cnt > /sys/power/wakeup_count || echo "wakeup_count write refused (event in progress)"; }
t0=$(cut -d' ' -f1 /proc/uptime); d0=$(date +%s)
echo mem > /sys/power/state; rc=$?
t1=$(cut -d' ' -f1 /proc/uptime); d1=$(date +%s)
echo "rc=$rc uptime_delta=$(echo "$t1 $t0" | awk '{print $1-$2}')s wall_delta=$((d1-d0))s pm_wakeup_irq=$(cat /sys/power/pm_wakeup_irq 2>&1)"
cat /sys/power/suspend_stats/success /sys/power/suspend_stats/fail /sys/power/suspend_stats/last_failed_dev 2>/dev/null | tr '\n' ' '; echo
dmesg | grep -v -E 'SHUB|nanohub|debug_work|pcie_msi_set_affinity' | grep -i -E 'PM: |suspend|resume|abort|fail|error|wakeup|Wakeup|wake|cause|Freez|Disabling|Enabling|CPU[0-9]' | head -80
echo "end $(date +%T)"
