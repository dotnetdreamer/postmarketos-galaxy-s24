#!/bin/sh
# grab a few frames from the PipeWire camera node as JPEGs (/tmp/grab_NNN.jpg), print the 3A line
unset LD_PRELOAD LD_LIBRARY_PATH
echo keep > /sys/power/wake_lock 2>/dev/null
rm -f /tmp/grab_*.jpg
su user -c "XDG_RUNTIME_DIR=/run/user/10000 timeout 20 gst-launch-1.0 -q pipewiresrc target-object=samsung_rear num-buffers=${1:-90} ! videoconvert ! videoscale ! video/x-raw,width=540,height=960 ! jpegenc quality=90 ! multifilesink location=/tmp/grab_%03d.jpg" >/tmp/grab.log 2>&1
ls /tmp/grab_*.jpg 2>/dev/null | wc -l
grep '\[stream\]' /tmp/bridge.log | tail -3
