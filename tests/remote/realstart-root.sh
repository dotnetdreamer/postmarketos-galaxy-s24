#!/bin/sh
# realstart-root.sh (run as root): join the seat's desktop session (its cgroup, which is how logind
# tells a session's programs apart), then as the user start s24-remote the way the quick setting does.
. /tmp/s24rt/real.sh
[ -n "$REAL_CGROUP" ] || { echo "no real session"; exit 1; }
echo $$ > /sys/fs/cgroup$REAL_CGROUP/cgroup.procs || exit 1
exec su user -s /bin/sh -c ". /tmp/s24rt/real.sh; export S24_REMOTE_CONFIG=/tmp/s24rt/real.conf; /usr/local/bin/s24-remote ${1:-on}"
