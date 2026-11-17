#!/usr/bin/python3
import socket, subprocess, time, re
sub = subprocess.Popen(['python3','/tmp/standin.py'], stdout=subprocess.PIPE, text=True)
standin = sub.stdout.readline().strip()
d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded','--device',standin,'--dpms','none','--socket','/tmp/st.sock'])
time.sleep(0.4)
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/st.sock'); s.sendall(b'on\n'); time.sleep(1.2)
def user(*c): return subprocess.run(['python3','/tmp/insess2.py']+list(c),stdout=subprocess.PIPE,text=True,timeout=15).stdout.strip()
try:
  dev=None
  for sn in user('qdbus6','org.kde.KWin','/org/kde/KWin/InputDevice','org.kde.KWin.InputDeviceManager.devicesSysNames').split():
    p='/org/kde/KWin/InputDevice/'+sn
    if 'mouse mode' in user('qdbus6','org.kde.KWin',p,'org.kde.KWin.InputDevice.name'): dev=p
  print('device', dev)
  print('before naturalScroll =', user('qdbus6','org.kde.KWin',dev,'org.kde.KWin.InputDevice.naturalScroll'))
  # gdbus Set with correct variant syntax
  r=user('gdbus','call','--session','--dest','org.kde.KWin','--object-path',dev,'--method',
         'org.freedesktop.DBus.Properties.Set','org.kde.KWin.InputDevice','naturalScroll','<true>')
  print('set ->', r or '(ok)')
  time.sleep(0.3)
  print('after naturalScroll =', user('qdbus6','org.kde.KWin',dev,'org.kde.KWin.InputDevice.naturalScroll'))
  # and tapToClick to prove config vs default: flip to false
  r=user('gdbus','call','--session','--dest','org.kde.KWin','--object-path',dev,'--method',
         'org.freedesktop.DBus.Properties.Set','org.kde.KWin.InputDevice','tapToClick','<false>')
  time.sleep(0.3)
  print('tapToClick after set false =', user('qdbus6','org.kde.KWin',dev,'org.kde.KWin.InputDevice.tapToClick'))
finally:
  s.sendall(b'off\n'); time.sleep(0.3); d.terminate(); d.wait()
  open('/tmp/mm-standin.fifo','w').write('quit\n'); sub.wait()
