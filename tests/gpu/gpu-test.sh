#!/bin/sh
# Galaxy S24 (Claude, 2026-11-15): run a browser in a private headless phoc that renders on the GPU
# exactly like the real session (gles2 via zink on the Xclipse Vulkan driver), as the phone user,
# without touching the real screen or login. Run as root in the postmarketOS root:
#   sh gpu-test.sh NAME URL SECONDS CMD...      -> /tmp/gpu-test/NAME.log, NAME.png
# CMD gets the URL appended. Logs keep the browser's stderr (console lines "S24BENCH ...").
name=$1; url=$2; secs=$3; shift 3
T=/tmp/gpu-test
mkdir -p $T/rt
cp /tmp/bench.html $T/bench.html 2>/dev/null
chown -R user:user $T; chmod 700 $T/rt
cat > $T/$name.inner <<EOF
cd $T
$* "$url" > $T/$name.log 2>&1 &
sleep $secs
grim $T/$name.png
pkill -f "user-data-dir=$T/ud-$name" ; pkill -f "/opt/vscode/code" ; sleep 2
EOF
chmod 755 $T/$name.inner
su -s /bin/sh user -c "export XDG_RUNTIME_DIR=$T/rt HOME=/home/user WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 MESA_LOADER_DRIVER_OVERRIDE=zink; dbus-run-session -- phoc -E $T/$name.inner > $T/$name.phoc.log 2>&1"
grep -a "S24BENCH" $T/$name.log | sed 's/.*S24BENCH/S24BENCH/; s/", source.*//' | sort -u
grep -a -iE "No usable sandbox|FATAL" $T/$name.log | head -3
