#!/bin/sh
# sessionend-root.sh (as root): Remote Desktop ended the way a logout or a shutdown ends it (SIGTERM,
# not `off`), whether it is still to stay on, and then `s24-remote login` run in the desktop session
# the way the login autostart runs it: it should come back by itself.
. /tmp/s24rt/real.sh
[ -n "$REAL_CGROUP" ] || { echo "no desktop session"; exit 1; }
flag=/home/user/.local/state/s24-remote/keep-on
st=/run/user/$(id -u user)/s24-remote/status.json
pid=$(cat /run/user/$(id -u user)/s24-remote/pid 2>/dev/null)
echo "running: ${pid:-none}, stays on: $([ -e $flag ] && echo yes || echo no)"
[ -n "$pid" ] && kill -TERM "$pid"
for i in $(seq 1 75); do [ -d /proc/$pid ] || break; sleep 0.2; done
echo "after SIGTERM: $([ -d /proc/$pid ] && echo 'still running' || echo ended), stays on: $([ -e $flag ] && echo yes || echo no)"
echo $$ > /sys/fs/cgroup$REAL_CGROUP/cgroup.procs || exit 1
su user -s /bin/sh -c ". /tmp/s24rt/real.sh; /usr/local/bin/s24-remote login" 2>&1
for i in $(seq 1 60); do
    grep -qE '"url": ?"https' "$st" 2>/dev/null && break
    sleep 2
done
echo "STATUS $(cat "$st" 2>/dev/null)"
