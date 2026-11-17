#!/bin/sh
# kernel wakeup sources that are active right now (they block suspend), plus the busiest ones
W=/sys/kernel/debug/wakeup_sources
echo "== ACTIVE now (active_since > 0):"
awk 'NR>1 && $6>0 {printf "   %-36s active %d ms, total %d ms, count %d\n", $1, $6, $7, $2}' $W
echo "== most time held since boot:"
awk 'NR>1 {print $7, $1, $2}' $W | sort -rn | head -8 | awk '{printf "   %-36s %d ms (%d times)\n", $2, $1, $3}'
echo "== userspace locks: [$(cat /sys/power/wake_lock)]"
