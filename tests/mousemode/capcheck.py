#!/usr/bin/python3
import socket, subprocess, time, re
sub = subprocess.Popen(['python3','/tmp/standin.py'], stdout=subprocess.PIPE, text=True)
standin = sub.stdout.readline().strip()
d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded','--device',standin,'--dpms','none','--socket','/tmp/cc.sock'])
time.sleep(0.4)
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/cc.sock'); s.sendall(b'on\n'); time.sleep(1.5)
def user(*c): return subprocess.run(['python3','/tmp/insess2.py']+list(c),stdout=subprocess.PIPE,text=True).stdout.strip()
try:
  for sn in user('qdbus6','org.kde.KWin','/org/kde/KWin/InputDevice','org.kde.KWin.InputDeviceManager.devicesSysNames').split():
    p='/org/kde/KWin/InputDevice/'+sn
    props=user('gdbus','call','--session','--dest','org.kde.KWin','--object-path',p,'--method','org.freedesktop.DBus.Properties.GetAll','org.kde.KWin.InputDevice')
    if 'mouse mode' not in props: continue
    for k in ('supportsNaturalScroll','naturalScrollEnabledByDefault','scrollTwoFinger','scrollTwoFingerEnabledByDefault','supportsScrollTwoFinger','tapFingerCount','supportsLmrTapButtonMap'):
      m=re.search(r"'%s': <([^>]*)>"%k,props); print('%s = %s'%(k, m.group(1) if m else 'absent'))
    # try setting it live via D-Bus and re-reading
    user('qdbus6','org.kde.KWin',p,'org.freedesktop.DBus.Properties.Set','org.kde.KWin.InputDevice','naturalScroll','true')
    time.sleep(0.3)
    print('after D-Bus set naturalScroll =', user('qdbus6','org.kde.KWin',p,'org.kde.KWin.InputDevice.naturalScroll'))
finally:
  s.sendall(b'off\n'); time.sleep(0.3); d.terminate(); d.wait()
  open('/tmp/mm-standin.fifo','w').write('quit\n'); sub.wait()
