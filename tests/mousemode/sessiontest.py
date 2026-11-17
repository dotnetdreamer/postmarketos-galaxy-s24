#!/usr/bin/python3
"""Session tests for the S24 mouse mode (Claude, 2026-11-15), run as root on the phone while its
panel is dark. A test s24-mouse-moded runs on the real socket with the stand-in touchscreen
(--dpms none); the agent runs as the session user with S24_MOUSE_MODE_IGNORE_DPMS=1. The touchpad
stays disabled in KWin (kcminputrc), so no pointer event reaches the session or wakes the panel.
Checked through KWin and KScreen: the scale, KWin's zoom, the keyboard mode, the saved state,
and that all of it comes back: on 'off', after the agent is killed, and from a login cleanup."""
import json, os, subprocess, sys, time

INSESS = ['python3', '/tmp/insess.py']
CLI = '/usr/local/bin/s24-mouse-mode'
HOME = '/home/user'
BEFORE = HOME + '/.cache/s24-mouse-mode/before.json'
ENV = ['env', 'S24_MOUSE_MODE_IGNORE_DPMS=1']
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print('%s %s %s' % ('PASS' if ok else 'FAIL', name, detail), flush=True)


def user(*cmd, timeout=20):
    r = subprocess.run(INSESS + list(cmd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return r.stdout.strip()


def scale():
    try:
        for o in json.loads(user('kscreen-doctor', '-j'))['outputs']:
            if o.get('type') == 7:
                return o.get('scale')
    except ValueError:
        return None


def zoom():
    out = user('qdbus6', 'org.kde.KWin', '/Effects', 'org.kde.kwin.Effects.supportInformation', 'zoom')
    z = {}
    for line in out.splitlines():
        if ':' in line:
            k, v = line.split(':', 1)
            z[k.strip()] = v.strip()
    return z


def vk_mode():
    return user('qdbus6', 'org.kde.KWin', '/VirtualKeyboard', 'org.kde.kwin.VirtualKeyboard.mode')


def kwinrc(key):
    return user('kreadconfig6', '--file', 'kwinrc', '--group', 'Effect-zoom', '--key', key, '--default', '-')


def status():
    return user(CLI, 'status')


def wait(pred, secs=8.0):
    end = time.time() + secs
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.25)
    return pred()


def baseline(tag):
    s, z, m = scale(), zoom(), vk_mode()
    check('%s: scale 2.75' % tag, s == 2.75, s)
    check('%s: zoom 1' % tag, float(z.get('targetZoom', 0)) == 1.0, z.get('targetZoom'))
    check('%s: zoom tracks proportionally again (0)' % tag, z.get('mouseTracking') == '0', z.get('mouseTracking'))
    check('%s: keyboard mode 1' % tag, m == '1', m)
    check('%s: no saved state left' % tag, not os.path.exists(BEFORE))
    check('%s: kwinrc MouseTracking/ZoomFactor/InitialZoom not left set' % tag,
          kwinrc('MouseTracking') == '-' and kwinrc('ZoomFactor') == '-' and kwinrc('InitialZoom') in ('-', '1'),
          (kwinrc('MouseTracking'), kwinrc('ZoomFactor'), kwinrc('InitialZoom')))


def on_state(tag):
    ok = wait(lambda: status() == 'on' and scale() == 1.5 and float(zoom().get('targetZoom', 0)) > 1.8)
    s, z, m = scale(), zoom(), vk_mode()
    check('%s: agent up' % tag, status() == 'on')
    check('%s: scale 1.5' % tag, s == 1.5, s)
    check('%s: zoom 2.75/1.5' % tag, abs(float(z.get('targetZoom', 0)) - 2.75 / 1.5) < 0.01, z.get('targetZoom'))
    check('%s: push tracking (2)' % tag, z.get('mouseTracking') == '2', z.get('mouseTracking'))
    check('%s: zoom step back to 1.2' % tag, z.get('zoomFactor') == '1.2', z.get('zoomFactor'))
    check('%s: keyboard for any input (2)' % tag, m == '2', m)
    try:
        b = json.load(open(BEFORE))
    except (OSError, ValueError):
        b = None
    check('%s: saved state' % tag, b and b.get('scale') == 2.75 and b.get('vk_mode') == 1 and b.get('screen') == 'DSI-1', b)
    return ok


def main():
    sub = subprocess.Popen(['python3', '/tmp/mm/standin.py'], stdout=subprocess.PIPE, text=True)
    standin = sub.stdout.readline().strip()
    check('stand-in touchscreen', standin.startswith('/dev/input/'), standin)
    dlog = open('/tmp/mm-session-daemon.log', 'w')
    d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded', '-v', '--device', standin, '--dpms', 'none'],
                         stdout=dlog, stderr=dlog)
    time.sleep(0.4)
    try:
        baseline('before')

        user(*ENV, CLI, 'on')
        on_state('on')

        # the keyboard opens: the screen is a touchscreen again, unzoomed; it closes: back
        user('qdbus6', 'org.kde.KWin', '/VirtualKeyboard', 'org.kde.kwin.VirtualKeyboard.forceActivate')
        typing = wait(lambda: float(zoom().get('targetZoom', 0)) == 1.0, 5)
        check('keyboard open: zoom 1', typing, zoom().get('targetZoom'))
        log = open('/tmp/mm-session-daemon.log').read()
        check('keyboard open: touch service paused', '<- pause' in log)
        user('qdbus6', 'org.kde.KWin', '/VirtualKeyboard', 'org.freedesktop.DBus.Properties.Set',
             'org.kde.kwin.VirtualKeyboard', 'active', 'false')
        back = wait(lambda: float(zoom().get('targetZoom', 0)) > 1.8, 5)
        check('keyboard closed: zoom back', back, zoom().get('targetZoom'))
        log = open('/tmp/mm-session-daemon.log').read()
        check('keyboard closed: touch service resumed', '<- resume' in log)

        # the three-finger hold ends it (stand-in fingers into the test daemon)
        with open('/tmp/mm-standin.fifo', 'w') as f:
            f.write('hold3 1200\n')
        gone = wait(lambda: status() == 'off' and scale() == 2.75, 10)
        check('three-finger hold: mouse mode ends', gone, status())
        time.sleep(1.5)
        baseline('after hold')

        # off from the command line (what the quick setting runs)
        user(*ENV, CLI, 'on')
        on_state('on again')
        user(CLI, 'off')
        wait(lambda: status() == 'off' and scale() == 2.75, 10)
        time.sleep(1.5)
        baseline('after off')

        # the agent dies: the touch service runs the cleanup as the user
        user(*ENV, CLI, 'on')
        on_state('on for kill')
        pid = user('sh', '-c', "pgrep -f 's24-mouse-mod[e] agent' | head -1")
        check('agent pid', pid.isdigit(), pid)
        os.kill(int(pid), 9)
        restored = wait(lambda: scale() == 2.75 and not os.path.exists(BEFORE), 12)
        check('agent killed: the touch service put it all back', restored, (scale(), os.path.exists(BEFORE)))
        time.sleep(1.5)
        baseline('after kill')

        # a run that never ended (phone off mid-run): the login cleanup puts it back
        user(*ENV, CLI, 'on')
        on_state('on for login')
        # both go at once (frozen first, so neither can clean up): nothing puts it back now
        pid = user('sh', '-c', "pgrep -f 's24-mouse-mod[e] agent' | head -1")
        if pid.isdigit():
            os.kill(int(pid), 19)          # SIGSTOP
        os.kill(d.pid, 9)
        d.wait()
        if pid.isdigit():
            os.kill(int(pid), 9)
        time.sleep(0.5)
        watchers = user('sh', '-c', "pgrep -f 's24-output-watc[h]' || true")
        check('the killed agent took its screen watcher along', watchers == '', watchers)
        check('stale run left its state', os.path.exists(BEFORE) and scale() == 1.5)
        d = subprocess.Popen(['/usr/local/sbin/s24-mouse-moded', '-v', '--device', standin, '--dpms', 'none'],
                             stdout=dlog, stderr=dlog)
        time.sleep(0.4)
        user(CLI, 'cleanup')
        time.sleep(1.5)
        baseline('after login cleanup')
    finally:
        user(CLI, 'off')
        time.sleep(1)
        user(CLI, 'cleanup')
        d.terminate()
        d.wait(timeout=5)
        try:
            with open('/tmp/mm-standin.fifo', 'w') as f:
                f.write('quit\n')
        except OSError:
            pass
        sub.wait(timeout=5)
    print('%d/%d passed' % (sum(ok for _, ok in results), len(results)))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == '__main__':
    sys.exit(main())
