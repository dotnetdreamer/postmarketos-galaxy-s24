#!/bin/sh
# what desktop runs, the panel state, and how udev/libinput see the touch device
ps -eo pid,etime,args | grep -E 'kwin_wayland|phosh|phoc|plasmashell|powerdevil|plasma-mobile|gnome-session' | grep -v grep | cut -c1-120
echo "--- panel:"; grep -A3 'DSI-1' /sys/kernel/debug/dri/1/state 2>/dev/null | grep -m1 'state:' ; cat /sys/class/drm/card*-DSI-1/enabled 2>/dev/null
for n in /sys/class/input/event*; do
  name=$(cat $n/device/name 2>/dev/null)
  case "$name" in sec_touchscreen|s2mps27-power-keys) echo "--- $n $name"; udevadm info -q property -p $n 2>/dev/null | grep -E 'ID_INPUT|LIBINPUT|WL_|DEVNAME' ;; esac
done
echo "--- key caps of sec_touchscreen:"
for n in /sys/class/input/input*; do [ "$(cat $n/name)" = sec_touchscreen ] && cat $n/capabilities/key && cat $n/capabilities/ev; done
