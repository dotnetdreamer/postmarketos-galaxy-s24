#!/bin/sh
# holdscreen.sh start|stop: a browser's screen that stays up (the helper fed heartbeats, no browser).
. /tmp/s24rt/t.sh
case "$1" in
start)
    ( while :; do printf '\000\000\000\000\012{"t":"hb"}'; sleep 2; done ) | \
        env -i $client_env setsid /usr/local/libexec/s24-remote/openandroiddex-pmos serve --width 1600 --height 900 --scale 1 \
        --name S24Remote >$T/hold.log 2>$T/hold.err &
    sleep 5
    grep -o '"output":"[^"]*"' $T/hold.log | head -1
    ;;
stop)
    for p in $(pgrep -x openandroiddex-p) $(pgrep -f "^/usr/local/libexec/s24-remote/openandroiddex-pmos serve"); do
        { tr '\0' '\n' </proc/$p/environ; } 2>/dev/null | grep -q "^WAYLAND_DISPLAY=s24rt-0" && kill $p
    done
    sleep 3
    ;;
esac
