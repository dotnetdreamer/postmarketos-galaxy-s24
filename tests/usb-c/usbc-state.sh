#!/bin/sh
# Galaxy S24: read-only look at USB-C, the dock or monitor on it, and DisplayPort. Run as root.
# Nothing here changes a setting; it holds a wake lock while it reads and drops it at the end.
unset LD_PRELOAD LD_LIBRARY_PATH
echo usbc-state > /sys/power/wake_lock
trap 'echo usbc-state > /sys/power/wake_unlock' EXIT

show() { for f in "$@"; do [ -r "$f" ] && printf '%s = %s\n' "$f" "$(timeout 2 cat "$f" 2>&1 | tr '\n' ' ' | head -c 200)"; done; }

echo "== USB lock state (0 unlocked, 1 locked, 2 locked + restricted, 3 never reported: no video out)"
show /sys/class/usb_notify/usb_control/usb_sl /sys/class/usb_notify/usb_control/disable

echo "== USB-C port (MAX77775)"
show /sys/class/typec/port0/data_role /sys/class/typec/port0/power_role \
	/sys/class/typec/port0/power_operation_mode \
	/sys/class/sec/ccic/usbpd_ids /sys/class/sec/ccic/cc_pin_status
[ -d /sys/class/typec/port0-partner ] && echo "partner attached" || echo "nothing attached"

echo "== DisplayPort"
show /sys/class/drm/card1-DP-1/status /sys/class/drm/card1-DP-1/enabled /sys/class/dp_sec/monitor_info
echo "modes: $(head -3 /sys/class/drm/card1-DP-1/modes 2>/dev/null | tr '\n' ' ')"

echo "== USB devices (host mode)"
for d in /sys/bus/usb/devices/*; do
	[ -f "$d/idVendor" ] || continue
	echo "$(basename "$d") $(cat "$d/idVendor"):$(cat "$d/idProduct") $(cat "$d/product" 2>/dev/null)"
done

echo "== Wired network"
ip -br link 2>/dev/null | grep -E '^(eth|enx)' || echo "no USB network adapter"

echo "== Charging"
show /sys/class/power_supply/battery/status /sys/class/power_supply/battery/capacity \
	/sys/class/power_supply/battery/current_now

echo "== Samsung's USB event log (last alternate mode and lock lines)"
grep -a -E 'alternate mode is|usb_sl_store|alt_is_support' /proc/usblog 2>/dev/null | tail -6
