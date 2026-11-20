#!/bin/sh
# mobilewake-root.sh (root): as mobiletest-root.sh (Wi-Fi off for 150 s, then on by itself), but with
# the screens' power turned on at the start and every 50 s (kscreen-doctor --dpms on, as the session's
# user): an idle lock screen goes dark after about a minute, and a dark screen sends no frames.
setsid nohup sh -c '
wake() { su user -s /bin/sh -c ". /tmp/s24rt/real.sh; QT_QPA_PLATFORM=wayland kscreen-doctor --dpms on >/dev/null 2>&1"; echo "dpms on $(date +%T)"; }
wake; sleep 3; date; nmcli radio wifi off; echo wifi off
sleep 50; wake; sleep 50; wake; sleep 50
nmcli radio wifi on; date; echo wifi on' >/tmp/s24rt/mobiletest.log 2>&1 </dev/null &
echo scheduled
