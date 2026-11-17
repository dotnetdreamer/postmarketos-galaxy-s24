#!/bin/sh
# Reboot the modem processor (cbd) and restart rild in the container; log /tmp/s24-radio-reset.log
exec > /tmp/s24-radio-reset.log 2>&1
A=/android
echo "== $(date '+%T') before: rild $(pgrep -f /vendor/bin/hw/rild) cbd $(pgrep -f /vendor/bin/cbd)"
chroot $A /system/bin/setprop ctl.restart cpboot-daemon
sleep 12
echo "== $(date '+%T') cbd now $(pgrep -f /vendor/bin/cbd)"
chroot $A /system/bin/setprop ctl.restart ril-daemon
sleep 8
echo "== $(date '+%T') rild now $(pgrep -f /vendor/bin/hw/rild)"
sleep 15
echo "== $(date '+%T') DONE"
