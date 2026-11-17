#!/usr/bin/python3
import socket, subprocess, time, re
sub = subprocess.Popen(['python3','/tmp/standin.py'], stdout=subprocess.PIPE, text=True)
standin = sub.stdout.readline().strip()
d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded','--device',standin,'--dpms','none','--socket','/tmp/sc.sock'])
time.sleep(0.4)
s=socket.socket(socket.AF_UNIX); s.connect('/tmp/sc.sock'); s.sendall(b'on\n')
def user(*c): return subprocess.run(['python3','/tmp/insess2.py']+list(c),stdout=subprocess.PIPE,text=True).stdout.strip()
try:
  for wait in (0.5, 2.0, 4.0):
    time.sleep(wait if wait==0.5 else wait-0.5)
    sysnames = user('qdbus6','org.kde.KWin','/org/kde/KWin/InputDevice','org.kde.KWin.InputDeviceManager.devicesSysNames').split()
    for sn in sysnames:
      p='/org/kde/KWin/InputDevice/'+sn
      props=user('gdbus','call','--session','--dest','org.kde.KWin','--object-path',p,'--method','org.freedesktop.DBus.Properties.GetAll','org.kde.KWin.InputDevice')
      if 'mouse mode' not in props: continue
      def g(k):
        m=re.search(r"'%s': <([^>]*)>"%k,props); return m.group(1) if m else '?'
      print('t=%.1f naturalScroll=%s tapToClick=%s scrollTwoFinger=%s enabled=%s'%(sum([0.5,wait][:1])+wait,g('naturalScroll'),g('tapToClick'),g('scrollTwoFinger'),g('enabled')))
finally:
  s.sendall(b'off\n'); time.sleep(0.3); d.terminate(); d.wait()
  open('/tmp/mm-standin.fifo','w').write('quit\n'); sub.wait()
