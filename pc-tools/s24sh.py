#!/usr/bin/env python3
"""PC side of the Galaxy S24 stage-0 init (Claude, 2026-11-14): runs shell commands over its busybox
telnetd (172.16.42.1:23 since init_boot v16, 2026-11-15; 172.16.43.1 before; S24_HOST overrides; no
login) and moves data with busybox nc.
  s24sh.py run "CMD" [timeout]       print CMD's output and exit status
  s24sh.py pull "PHONE-CMD" OUT      stream PHONE-CMD's stdout (e.g. "dd if=/dev/sda1") into OUT
  s24sh.py push LOCAL "PHONE-CMD"    stream LOCAL into PHONE-CMD's stdin (e.g. "cat > /tmp/x")
  s24sh.py put LOCAL REMOTE          small file, base64 through the shell (prints its md5)
Transfers keep their telnet session open: busybox telnetd hangs up background jobs when the shell
exits, so the phone-side nc runs in the foreground of a live session.
"""
import socket
import sys
import time

HOST = __import__("os").environ.get("S24_HOST", "172.16.42.1")
PORT = int(__import__("os").environ.get("S24_PORT", "23"))
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
NL = chr(10)


def strip_iac(sock, data):
    out, i = bytearray(), 0
    while i < len(data):
        b = data[i]
        if b != IAC or i + 1 >= len(data):
            out.append(b)
            i += 1
            continue
        c = data[i + 1]
        if c in (DO, DONT, WILL, WONT) and i + 2 < len(data):
            if c == DO:
                sock.sendall(bytes([IAC, WONT, data[i + 2]]))
            elif c == WILL:
                sock.sendall(bytes([IAC, DONT, data[i + 2]]))
            i += 3
        elif c == SB:
            j = data.find(bytes([IAC, SE]), i)
            i = j + 2 if j >= 0 else len(data)
        elif c == IAC:
            out.append(IAC)
            i += 2
        else:
            i += 2
    return bytes(out)


def _read_until(t, marker, timeout):
    t.settimeout(1)
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            d = t.recv(65536)
        except socket.timeout:
            if marker in buf:
                break
            continue
        if not d:
            break
        buf += strip_iac(t, d)
        if marker in buf and buf.rstrip().endswith(b"#"):
            break
    return buf


def run(cmd, timeout=120):
    t = socket.create_connection((HOST, PORT), timeout=10)
    # markers are split in the typed text so the terminal echo never matches them
    t.sendall(("stty -echo 2>/dev/null; echo __S24_\"\"BEGIN__; " + cmd + NL + "echo __S24_\"\"END__ $?" + NL + "exit" + NL).encode())
    buf = _read_until(t, b"__S24_END__", timeout)
    t.close()
    text = buf.decode(errors="replace").replace("\r\n", NL)
    if "__S24_BEGIN__" in text:
        text = text.split("__S24_BEGIN__", 1)[1].lstrip(NL)
    out, _, rest = text.partition("__S24_END__")
    rc = rest.strip().split()[0] if rest.strip() else "?"
    return out, rc


def _session(cmd):
    t = socket.create_connection((HOST, PORT), timeout=10)
    t.sendall(("stty -echo 2>/dev/null; " + cmd + "; echo __S24_\"\"XFER__ $?" + NL).encode())
    time.sleep(1.5)
    return t


def _finish(t, timeout=120):
    buf = _read_until(t, b"__S24_XFER__", timeout)
    try:
        t.sendall(("exit" + NL).encode())
    except OSError:
        pass
    t.close()
    text = buf.decode(errors="replace")
    return text.split("__S24_XFER__", 1)[1].split()[0] if "__S24_XFER__" in text else "?"


def pull(phone_cmd, path, port=5555):
    t = _session("%s | nc -l -p %d" % (phone_cmd, port))
    s = socket.create_connection((HOST, port), timeout=30)
    total, t0 = 0, time.time()
    with open(path, "wb") as f:
        while True:
            d = s.recv(1 << 20)
            if not d:
                break
            f.write(d)
            total += len(d)
    s.close()
    rc = _finish(t)
    dt = max(time.time() - t0, 0.001)
    print("%s: %d bytes in %.1f s (%.1f MB/s), phone exit %s" % (path, total, dt, total / dt / 1e6, rc))
    return total


def push(local, phone_cmd, port=5556):
    t = _session("nc -l -p %d < /dev/null | %s" % (port, phone_cmd))  # nc must not wait on the session's stdin
    s = socket.create_connection((HOST, port), timeout=30)
    total, t0 = 0, time.time()
    with open(local, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            s.sendall(chunk)
            total += len(chunk)
    s.shutdown(socket.SHUT_WR)
    s.settimeout(3)  # busybox nc -l never closes its side: stop waiting soon
    try:
        while s.recv(65536):
            pass
    except OSError:
        pass
    s.close()
    rc = _finish(t, timeout=600)
    dt = max(time.time() - t0, 0.001)
    print("%s -> phone: %d bytes in %.1f s (%.1f MB/s), phone exit %s" % (local, total, dt, total / dt / 1e6, rc))
    return total


def put(local, remote):
    """small files: base64 through the shell itself (no nc, no hang)"""
    import base64
    data = base64.encodebytes(open(local, "rb").read()).decode()
    out, rc = run("base64 -d > '%s' <<'__S24EOF__'" % remote + NL + data + "__S24EOF__" + NL +
                  "md5sum '%s'" % remote, timeout=120)
    print(out.strip(), "[exit %s]" % rc)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        out, rc = run(sys.argv[2], timeout=int(sys.argv[3]) if len(sys.argv) > 3 else 120)
        sys.stdout.write(out)
        print("[exit %s]" % rc)
    elif len(sys.argv) == 4 and sys.argv[1] == "pull":
        pull(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "put":
        put(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "push":
        push(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
