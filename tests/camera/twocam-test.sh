#!/bin/sh
# full setup with the two-camera build, then back / front / switch tests
export BRIDGE=serve S24CAM_BIN=/root/s24-cam.pw
sh /home/user/cambridge13.sh 2>&1 | grep -E 'IThermal|bound|provider=|abort|pwlat\] [0-9]|first frame' | tail -6
echo "== nodes:"; grep -E '^\[pw\] node|^camera device' /tmp/bridge.log | head -6
P="su user -c"; RT="XDG_RUNTIME_DIR=/run/user/10000"
sleep 7
echo "== front camera alone (6 s):"; su user -c "$RT /home/user/pwlat samsung_front 6" 2>&1 | grep -E 'first frame|frames,'
grep -E 'camera .* open|camera ready|switch|HAL default' /tmp/bridge.log | tail -3 | sed 's/^/   /'
echo "== back then front, back to back (switch):"
su user -c "$RT /home/user/pwlat samsung_rear 4" 2>&1 | grep -E 'first frame|frames,'
su user -c "$RT /home/user/pwlat samsung_front 4" 2>&1 | grep -E 'first frame|frames,'
grep -E 'switch|camera .* open|camera ready|closed' /tmp/bridge.log | tail -5 | sed 's/^/   /'
echo "== app-visible properties:"
su user -c "$RT timeout 6 gst-device-monitor-1.0 Video/Source" 2>/dev/null | grep -E 'name  :|api.libcamera.location|node.name' | head -8
echo "alive: provider=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1) s24-cam=$(pgrep -f 's24-cam[^ ]* serve'|head -1)"
