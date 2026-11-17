#!/bin/sh
# Galaxy S24 (Claude, 2026-11-15): official VS Code in a private headless GPU phoc (2340x1080, like
# the panel turned sideways) as the user, throwaway profile, a 6000-line file open; then measure over
# CDP: WebGL renderer + 4 s of wheel scrolling (frame times) + CPU time of all VS Code processes.
# Only this test's own processes are killed (matched by their profile path), never the user's.
#   sh vsc-gpu-test.sh NAME GPU|NOGPU [extra code flags...]   -> /tmp/gpu-test/vNAME.*
name=$1; mode=$2; shift 2
T=/tmp/gpu-test; mkdir -p $T/rt $T/vproj; cp /tmp/cdp-eval.mjs /tmp/vsc-scroll.js $T/
[ -f $T/vproj/big.js ] || awk 'BEGIN{for(i=1;i<=6000;i++) printf("export function fn%d(a, b) { const s = `line %d ${a}`; return s.length + b * %d; } // padding text for width\n", i, i, i)}' > $T/vproj/big.js
mkdir -p $T/vud-$name/User
printf '{"security.workspace.trust.enabled": false, "workbench.startupEditor": "none", "editor.smoothScrolling": false, "window.newWindowDimensions": "maximized"}\n' > $T/vud-$name/User/settings.json
printf '[output:HEADLESS-1]\nmode = 2340x1080\nscale = 1\n' > $T/phoc-test.ini
chown -R user:user $T; chmod 700 $T/rt
G=/var/lib/flatpak/runtime/org.freedesktop.Platform.GL.default/aarch64/25.08/active/files
if [ "$mode" = GPU ]; then
	envs="export __EGL_VENDOR_LIBRARY_FILENAMES=$G/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_DRIVERS_PATH=$G/lib/dri GBM_BACKENDS_PATH=$G/lib/gbm VK_DRIVER_FILES=/opt/xclipse-glibc/xclipse_icd.json VK_ICD_FILENAMES=/opt/xclipse-glibc/xclipse_icd.json VK_LOADER_DEBUG=error,warn,driver"
	gflags=""
else
	envs=":"
	gflags="--disable-gpu"
fi
# VS Code reads ~/.vscode/argv.json whatever the profile dir: give each test its own home
mkdir -p $T/home-$name/.vscode
if [ "$mode" = GPU ]; then
	printf '{"disable-chromium-sandbox": true, "enable-crash-reporter": false}
' > $T/home-$name/.vscode/argv.json
else
	printf '{"disable-hardware-acceleration": true, "disable-chromium-sandbox": true, "enable-crash-reporter": false}
' > $T/home-$name/.vscode/argv.json
fi
chown -R user:user $T/home-$name
cat > $T/v$name.inner <<IN
$envs
export HOME=$T/home-$name
unset LD_PRELOAD
/opt/vscode/bin/code --verbose --user-data-dir $T/vud-$name --extensions-dir $T/vext --remote-debugging-port=9223 --ozone-platform=wayland --enable-wayland-ime=1 --wayland-text-input-version=3 $gflags $* $T/vproj/big.js > $T/v$name.log 2>&1 &
sleep 16
# CPU ticks (utime+stime) of every process descended from this script: VS Code and its helpers only
cputicks() { awk -v root=\$\$ 'FNR==1{ s=\$0; i=0; for(k=length(s);k>0;k--) if(substr(s,k,1)==")"){i=k;break}; split(substr(s,i+2),a," "); pid=\$1+0; pp[pid]=a[2]+0; t[pid]=a[12]+a[13] } END{ for(p in pp){ c=p+0; while(c>1){ if(c==root && p+0!=root){print p, t[p]; break}; c=pp[c] } } }' /proc/[0-9]*/stat 2>/dev/null; }
cputicks > $T/v$name.t0; sys0=\$(awk '/^cpu /{print \$2+\$3+\$4+\$7+\$8}' /proc/stat)
node $T/cdp-eval.mjs 9223 $T/vsc-scroll.js vscode-file > $T/v$name.scroll 2>&1
cputicks > $T/v$name.t1; sys1=\$(awk '/^cpu /{print \$2+\$3+\$4+\$7+\$8}' /proc/stat)
echo "vscode_cpu_ticks=\$(awk "FNR==NR{a[\\$1]=\\$2;next} {d+=\\$2-((\\$1 in a)?a[\\$1]:0)} END{print d+0}" $T/v$name.t0 $T/v$name.t1) system_busy_ticks=\$((sys1-sys0)) (100 ticks = 1 core-second)" >> $T/v$name.scroll
grim $T/v$name.png
pkill -f "vud-$name"; sleep 3
IN
chmod 755 $T/v$name.inner
su -s /bin/sh user -c "export XDG_RUNTIME_DIR=$T/rt HOME=/home/user WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 MESA_LOADER_DRIVER_OVERRIDE=zink; timeout 120 dbus-run-session -- phoc -C $T/phoc-test.ini -E $T/v$name.inner > $T/v$name.phoc.log 2>&1"
pkill -f "vud-$name" 2>/dev/null
cat $T/v$name.scroll
grep -a -oE "ozone-platform=[a-z]+" $T/v$name.log | sort -u | head -2
