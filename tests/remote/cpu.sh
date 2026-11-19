#!/bin/sh
# cpu.sh SECONDS: the helper's (and s24-remote's, and KWin's) CPU over SECONDS, in % of one core.
S=${1:-10}
hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
pids=""
for p in $(pgrep -x openandroiddex-p) $(pgrep -f "^/usr/local/libexec/s24-remote/openandroiddex-pmos serve"); do pids="$pids $p"; done
for p in $(pgrep -x s24-remote); do pids="$pids $p"; done
for p in $(pgrep -f "kwin_wayland --virtual --socket s24rt-0"); do pids="$pids $p"; done
ticks() { awk '{print $14 + $15}' /proc/$1/stat 2>/dev/null || echo 0; }
for p in $pids; do eval "a$p=$(ticks $p)"; done
sleep $S
for p in $pids; do
    b=$(ticks $p); eval "a=\$a$p"
    name=$(tr '\0' ' ' </proc/$p/cmdline | cut -c1-60)
    echo "$p $(awk -v d=$((b - a)) -v hz=$hz -v s=$S 'BEGIN{printf "%5.1f%%", 100*d/hz/s}') $name"
done | sort -u
