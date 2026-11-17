#!/bin/sh
# when did the PC reset or suspend the USB link? (uptime seconds -> UTC)
now=$(cut -d' ' -f1 /proc/uptime | cut -d. -f1); t=$(date -u +%s)
dmesg | grep -E 'dwc3_gadget_reset_interrupt|dwc3_gadget_vbus_draw: suspend' | while read -r line; do
  up=$(echo "$line" | sed -E 's/^\[ *([0-9]+)\..*/\1/')
  what=$(echo "$line" | grep -q reset_interrupt && echo reset || echo suspend)
  echo "$(date -u -d @$((t - now + up)) +%T 2>/dev/null || echo up=$up) $what"
done | uniq -c | tail -15
