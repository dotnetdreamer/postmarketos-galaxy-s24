#!/bin/sh
unset LD_PRELOAD LD_LIBRARY_PATH
A=/android
chroot $A /system/bin/logcat -b crash -d 2>/dev/null | tail -40 | cut -c1-200
echo "== init/provider restarts:"
chroot $A /system/bin/logcat -b all -d 2>/dev/null | grep -E "camera.provider|sec-camera-provider|exited|died|Service .* (exited|died)" | grep -iE "exit|die|kill|start" | tail -12 | cut -c1-200
