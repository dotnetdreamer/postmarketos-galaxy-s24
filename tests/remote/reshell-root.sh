#!/bin/sh
# reshell-root.sh (root): join the desktop session's cgroup, then as the user replace plasmashell.
. /tmp/s24rt/real.sh
[ -n "$REAL_CGROUP" ] && [ -n "$REAL_PLASMASHELL" ] || { echo "no real session"; exit 1; }
echo $$ > /sys/fs/cgroup$REAL_CGROUP/cgroup.procs || exit 1
exec su user -s /bin/sh -c "python3 /tmp/s24rt/reshell.py $REAL_PLASMASHELL"
