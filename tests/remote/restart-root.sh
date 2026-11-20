#!/bin/sh
# restart-root.sh (as root): Remote Desktop off and on again in the desktop session, the way the
# quick setting would (the session's cgroup, its environment, the default settings), then its status
# once the internet address works. For trying a newly installed s24-remote on a running phone.
. /tmp/s24rt/real.sh
[ -n "$REAL_CGROUP" ] || { echo "no desktop session"; exit 1; }
echo $$ > /sys/fs/cgroup$REAL_CGROUP/cgroup.procs || exit 1
st=/run/user/$(id -u user)/s24-remote/status.json
su user -s /bin/sh -c ". /tmp/s24rt/real.sh; /usr/local/bin/s24-remote off; /usr/local/bin/s24-remote on" 2>&1
for i in $(seq 1 60); do
    grep -qE '"url": ?"https' "$st" 2>/dev/null && break
    sleep 2
done
echo "STATUS $(cat "$st" 2>/dev/null)"
