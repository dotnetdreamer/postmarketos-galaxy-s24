#!/usr/bin/env python3
"""Apply Android's audio routes (vendor mixer_paths.xml) to the Galaxy S24's ALSA card, like the
stock audio HAL does (Claude, 2026-11-14).
  s24-mixer.py defaults            all top-level <ctl> values (the HAL's initial state)
  s24-mixer.py path NAME [...]     a named <path>, nested paths expanded
  s24-mixer.py show NAME           print what a path would set
Values go through "amixer -c CARD cset name=..." (one batch via amixer -s)."""
import subprocess
import sys
import xml.etree.ElementTree as ET

XML = "/android/vendor/etc/mixer_paths.xml"
CARD = "0"

root = ET.parse(XML).getroot()
paths = {}
for p in root.iter("path"):
    if p.get("name") and len(p):
        paths.setdefault(p.get("name"), p)


def ctls_of(name, out, seen=None):
    seen = seen or set()
    if name in seen:
        return out
    seen.add(name)
    p = paths.get(name)
    if p is None:
        raise SystemExit("no such path: " + name)
    for c in p:
        if c.tag == "path":
            ctls_of(c.get("name"), out, seen)
        elif c.tag == "ctl":
            out.append(c)
    return out


def value(c):
    v = c.get("value")
    if v is None:
        v = c.get("id", "")
    return v


def apply(ctls):
    lines = []
    for c in ctls:
        name = c.get("name").replace("'", "")
        v = value(c)
        if c.get("id") is not None and c.get("value") is not None:
            continue  # per-element array writes: not needed for routing, skipped
        lines.append("cset 'name=%s' '%s'" % (name, v))
    r = subprocess.run(["amixer", "-c", CARD, "-q", "-s"], input="\n".join(lines) + "\n",
                       text=True, capture_output=True)
    bad = [l for l in r.stderr.splitlines() if l.strip()]
    print("applied %d controls, %d errors" % (len(lines), len(bad)))
    for l in bad[:10]:
        print("  " + l)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "defaults":
        apply([c for c in root if c.tag == "ctl"])
    elif len(sys.argv) >= 3 and sys.argv[1] == "path":
        out = []
        for n in sys.argv[2:]:
            ctls_of(n, out)
        apply(out)
    elif len(sys.argv) == 3 and sys.argv[1] == "show":
        for c in ctls_of(sys.argv[2], []):
            print("%s = %s" % (c.get("name"), value(c)))
    else:
        sys.exit(__doc__)
