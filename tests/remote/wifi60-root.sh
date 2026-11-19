#!/bin/sh
# mobiletest-root.sh (root): Wi-Fi off for 60 s, then on again by itself (SSH over Wi-Fi goes with
# it), so that a browser can be tried on the phone's mobile data alone.
setsid nohup sh -c 'sleep 3; date; nmcli radio wifi off; echo wifi off; sleep 60; nmcli radio wifi on; date; echo wifi on' >/tmp/s24rt/mobiletest.log 2>&1 </dev/null &
echo scheduled
