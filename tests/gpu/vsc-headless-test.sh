#!/bin/sh
# Galaxy S24 (Claude, 2026-11-15): start official VS Code inside a private headless phoc as the phone
# user and screenshot it, without touching the phone's real screen or login. Run as root in the
# postmarketOS root:  sh vsc-headless-test.sh   -> /tmp/vsc-test/shot.png, /tmp/vsc-test/code.log
T=/tmp/vsc-test
rm -rf $T
mkdir -p $T/rt $T/proj
printf 'console.log("hello from node", process.version);\n' > $T/proj/hello.js
printf '# vsc test\n' > $T/proj/README.md
chown -R user:user $T
chmod 700 $T/rt
cat > $T/inner.sh <<'EOF'
cd /tmp/vsc-test
/usr/local/bin/code --verbose --user-data-dir /tmp/vsc-test/ud --extensions-dir /tmp/vsc-test/ext /tmp/vsc-test/proj > /tmp/vsc-test/code.log 2>&1 &
sleep 30
grim /tmp/vsc-test/shot.png
pkill -f /opt/vscode/code
sleep 2
EOF
chmod 755 $T/inner.sh
su -s /bin/sh user -c "export XDG_RUNTIME_DIR=$T/rt WLR_BACKENDS=headless WLR_RENDERER=pixman HOME=/home/user; dbus-run-session -- phoc -E /tmp/vsc-test/inner.sh > $T/phoc.log 2>&1"
ls -la $T/shot.png 2>&1
