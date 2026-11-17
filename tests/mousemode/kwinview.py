#!/usr/bin/python3
"""Make the mouse-mode touchpad (test daemon, stand-in touchscreen, no input sent) and print how
KWin sees it (Claude, 2026-11-15). Run as root on the phone."""
import socket, subprocess, time
sub = subprocess.Popen(['python3', '/tmp/mm/standin.py'], stdout=subprocess.PIPE, text=True)
standin = sub.stdout.readline().strip()
d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded', '--device', standin, '--dpms', 'none', '--socket', '/tmp/mmview.sock'])
time.sleep(0.4)
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/mmview.sock'); s.sendall(b'on\n'); time.sleep(1.0)
def user(*c):
    return subprocess.run(['python3', '/tmp/insess.py'] + list(c), stdout=subprocess.PIPE, text=True).stdout.strip()
try:
    for sysname in user('qdbus6', 'org.kde.KWin', '/org/kde/KWin/InputDevice', 'org.kde.KWin.InputDeviceManager.devicesSysNames').split():
        path = '/org/kde/KWin/InputDevice/' + sysname
        props = user('gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', path, '--method',
                     'org.freedesktop.DBus.Properties.GetAll', 'org.kde.KWin.InputDevice')
        if 'mouse mode' not in props:
            continue
        print(sysname)
        import re
        for key in ('name', 'enabled', 'touchpad', 'pointer', 'touch', 'tapToClick', 'tapAndDrag', 'tapDragLock',
                    'naturalScroll', 'scrollTwoFinger', 'lmrTapButtonMap', 'disableWhileTyping', 'size',
                    'pointerAcceleration', 'tapFingerCount', 'clickMethodClickfinger', 'supportsDisableWhileTyping'):
            m = re.search(r"'%s': <([^>]*)>" % key, props)
            print('  %s = %s' % (key, m.group(1) if m else '?'))
    print('udev:', subprocess.run(['sh', '-c', "for e in /sys/class/input/event*; do n=$(cat $e/device/name); [ \"$n\" = 'S24 mouse mode touchpad' ] && udevadm info $e | grep -E 'ID_INPUT|INTEGRATION'; done"], stdout=subprocess.PIPE, text=True).stdout)
finally:
    s.sendall(b'off\n'); time.sleep(0.3); s.close()
    d.terminate(); d.wait()
    open('/tmp/mm-standin.fifo', 'w').write('quit\n'); sub.wait()
