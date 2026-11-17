#!/bin/sh
S=/sys/power/suspend_stats
for f in success fail failed_freeze failed_prepare failed_suspend failed_suspend_late failed_suspend_noirq failed_resume last_failed_dev last_failed_errno last_failed_step; do
	printf "%s=%s  " $f "$(cat $S/$f 2>/dev/null | tr '\n' ' ')"; done; echo
echo "== sleep daemon, this boot:"; sed -n '/03:24:58 started/,$p' /var/log/s24-power.log | grep -vE 'screen (on|off)$' | head -20
echo "== kernel, around the attempts:"
dmesg | grep -iE 'PM: (suspend|Some devices|Device .*failed|dpm_run|failed)|Freezing|freeze.*fail|abort|refused|Wakeup pending|wakeup source.*active|busy' | head -24 | cut -c1-170
