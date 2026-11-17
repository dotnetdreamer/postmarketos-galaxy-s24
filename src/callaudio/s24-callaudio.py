#!/usr/bin/env python3
"""Galaxy S24 (postmarketOS) call-audio helper (Claude, 2026-11-16).

The voice media of a circuit-switched call does NOT pass through Linux: the modem and the ABOX
audio DSP exchange PCM over an internal link. This helper only does what Samsung's audio HAL does
AROUND that, on the Linux side (it does NOT talk to the modem; rild rejects non-stock clients):
  - put the ABOX DSP in "in call" mode,
  - apply the stock incall routing for the earpiece or speaker (read live from the vendor
    mixer_paths.xml, exactly the paths the HAL applies),
  - open and start the three call PCM nodes (104 Rx, 123 Tx, 131 Ref) so their DMA/ASRC run;
    nothing is written or read.
On teardown it undoes all of that and re-applies the normal speaker+mic media route, so ordinary
PulseAudio sound works again.

Controls go through libasound (snd_ctl_elem_write), NOT `amixer`: several ABOX integer controls
report min=0,max=0 and amixer would clamp them to 0.

The s24-mmd bridge runs this once per call and drives it on stdin, one command per line:
  device earpiece|speaker     (before start)
  mute 0|1                    mic mute
  start                       bring call audio up
  stop  (or EOF)              tear it down and exit
Standalone:  s24-callaudio.py parse earpiece   # list what would be applied, touch nothing
             s24-callaudio.py dry earpiece      # apply, hold 3 s, tear down (NO call; safe check)
"""
import ctypes
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

XML = "/android/vendor/etc/mixer_paths.xml"
CARD = "hw:0"
SHARE = "/usr/local/share/s24"
MEDIA_FILES = [SHARE + "/s24-speaker.amixer", SHARE + "/s24-mic.amixer"]

# ALSA constants
IFACE_MIXER = 2
T_BOOL, T_INT, T_ENUM, T_INT64 = 1, 2, 3, 5
STREAM_PLAYBACK, STREAM_CAPTURE = 0, 1
FMT_S16_LE = 2
ACCESS_RW_INTERLEAVED = 3

# the three loop PCM nodes: (device, stream, channels)
NODE_RX, NODE_TX, NODE_REF = 104, 123, 131


def log(msg):
    print("callaudio: " + msg, flush=True)


a = ctypes.CDLL("libasound.so.2")

# ctypes defaults every argument to 32-bit int, which TRUNCATES the 64-bit opaque handles ALSA
# passes around. Declare argtypes/restype for everything, or nothing works on aarch64.
P = ctypes.c_void_p
PP = ctypes.POINTER(ctypes.c_void_p)
I, U, L, UL = ctypes.c_int, ctypes.c_uint, ctypes.c_long, ctypes.c_ulong
S = ctypes.c_char_p
_SIG = {
    "snd_strerror": (S, [I]),
    "snd_ctl_open": (I, [PP, S, I]), "snd_ctl_close": (I, [P]),
    "snd_ctl_elem_info": (I, [P, P]), "snd_ctl_elem_read": (I, [P, P]),
    "snd_ctl_elem_write": (I, [P, P]),
    "snd_ctl_elem_info_malloc": (I, [PP]), "snd_ctl_elem_value_malloc": (I, [PP]),
    "snd_ctl_elem_info_free": (None, [P]), "snd_ctl_elem_value_free": (None, [P]),
    "snd_ctl_elem_info_set_interface": (None, [P, I]),
    "snd_ctl_elem_info_set_name": (None, [P, S]),
    "snd_ctl_elem_info_set_item": (None, [P, U]),
    "snd_ctl_elem_info_get_type": (U, [P]), "snd_ctl_elem_info_get_count": (U, [P]),
    "snd_ctl_elem_info_get_items": (U, [P]), "snd_ctl_elem_info_get_item_name": (S, [P]),
    "snd_ctl_elem_value_set_interface": (None, [P, I]),
    "snd_ctl_elem_value_set_name": (None, [P, S]),
    "snd_ctl_elem_value_set_integer": (None, [P, U, L]),
    "snd_ctl_elem_value_set_boolean": (None, [P, U, L]),
    "snd_ctl_elem_value_set_enumerated": (None, [P, U, U]),
    "snd_ctl_elem_value_get_integer": (L, [P, U]),
    "snd_pcm_open": (I, [PP, S, I, I]), "snd_pcm_close": (I, [P]),
    "snd_pcm_hw_params_malloc": (I, [PP]), "snd_pcm_hw_params_any": (I, [P, P]),
    "snd_pcm_hw_params_set_access": (I, [P, P, U]), "snd_pcm_hw_params_set_format": (I, [P, P, I]),
    "snd_pcm_hw_params_set_channels": (I, [P, P, U]),
    "snd_pcm_hw_params_set_rate": (I, [P, P, U, I]),
    "snd_pcm_hw_params_set_period_size": (I, [P, P, UL, I]),
    "snd_pcm_hw_params_set_periods": (I, [P, P, U, I]), "snd_pcm_hw_params": (I, [P, P]),
    "snd_pcm_sw_params_malloc": (I, [PP]), "snd_pcm_sw_params_current": (I, [P, P]),
    "snd_pcm_sw_params_get_boundary": (I, [P, ctypes.POINTER(UL)]),
    "snd_pcm_sw_params_set_start_threshold": (I, [P, P, UL]),
    "snd_pcm_sw_params_set_stop_threshold": (I, [P, P, UL]),
    "snd_pcm_sw_params": (I, [P, P]), "snd_pcm_prepare": (I, [P]),
    "snd_pcm_start": (I, [P]), "snd_pcm_drop": (I, [P]),
}
for _fn, (_res, _args) in _SIG.items():
    getattr(a, _fn).restype = _res
    getattr(a, _fn).argtypes = _args


def ck(rc, what):
    if rc < 0:
        log("%s: %s" % (what, a.snd_strerror(rc).decode()))
    return rc


class Ctl:
    def __init__(self):
        self.h = ctypes.c_void_p()
        ck(a.snd_ctl_open(ctypes.byref(self.h), CARD.encode(), 0), "ctl open")

    def close(self):
        if self.h:
            a.snd_ctl_close(self.h)
            self.h = None

    def _enum_index(self, info, name, want):
        items = a.snd_ctl_elem_info_get_items(info)
        for j in range(items):
            a.snd_ctl_elem_info_set_item(info, j)
            if a.snd_ctl_elem_info(self.h, info) < 0:
                break
            nm = a.snd_ctl_elem_info_get_item_name(info)
            if nm and nm.decode() == want:
                return j
        try:
            return int(want)
        except ValueError:
            log("enum '%s' has no item '%s'" % (name, want))
            return None

    def set(self, name, value, elem_id=None):
        """Set an ABOX/codec control by name. value may be a scalar or a space-separated list.
        A fresh info/value struct each time: once resolved, the struct caches the numid and would
        otherwise re-target that same control on the next (differently named) call."""
        info = ctypes.c_void_p()
        val = ctypes.c_void_p()
        a.snd_ctl_elem_info_malloc(ctypes.byref(info))
        a.snd_ctl_elem_value_malloc(ctypes.byref(val))
        try:
            a.snd_ctl_elem_info_set_interface(info, IFACE_MIXER)
            a.snd_ctl_elem_info_set_name(info, name.encode())
            if a.snd_ctl_elem_info(self.h, info) < 0:
                return  # control not on this kernel: skip quietly
            typ = a.snd_ctl_elem_info_get_type(info)
            count = a.snd_ctl_elem_info_get_count(info)
            a.snd_ctl_elem_value_set_interface(val, IFACE_MIXER)
            a.snd_ctl_elem_value_set_name(val, name.encode())
            a.snd_ctl_elem_read(self.h, val)               # keep current for untouched elements
            idxs = [int(elem_id)] if elem_id is not None else range(count)
            if typ == T_ENUM:
                # an enum item name can itself contain spaces ("SPUS OUT8"): never split it
                idx = self._enum_index(info, name, str(value).strip())
                if idx is not None:
                    for i in idxs:
                        a.snd_ctl_elem_value_set_enumerated(val, i, idx)
            else:
                parts = str(value).split()                 # "0 2 4 6" = one value per element
                for i in idxs:
                    v = parts[i] if i < len(parts) else parts[-1]
                    if typ == T_BOOL:
                        a.snd_ctl_elem_value_set_boolean(val, i, 1 if v in ("1", "on", "true",
                                                                            "enable", "Enabled") else 0)
                    else:
                        try:
                            a.snd_ctl_elem_value_set_integer(val, i, int(v))
                        except ValueError:
                            log("control '%s' wants a number, got '%s'" % (name, v))
                            return
            ck(a.snd_ctl_elem_write(self.h, val), "write " + name)
        finally:
            a.snd_ctl_elem_info_free(info)
            a.snd_ctl_elem_value_free(val)


# ---- PCM nodes ------------------------------------------------------------------------------
def pcm_open_start(dev, stream, channels):
    """Open hw:0,<dev>, configure like the stock HAL (48k S16 480x4, never stop on xrun), start."""
    h = ctypes.c_void_p()
    name = "hw:0,%d" % dev
    if ck(a.snd_pcm_open(ctypes.byref(h), name.encode(), stream, 0), "open " + name) < 0:
        return None
    hw = ctypes.c_void_p()
    a.snd_pcm_hw_params_malloc(ctypes.byref(hw))
    a.snd_pcm_hw_params_any(h, hw)
    a.snd_pcm_hw_params_set_access(h, hw, ACCESS_RW_INTERLEAVED)
    a.snd_pcm_hw_params_set_format(h, hw, FMT_S16_LE)
    ch = channels
    if a.snd_pcm_hw_params_set_channels(h, hw, ch) < 0 and ch != 2:
        ch = 2                                              # Tx falls back from quad-mic to stereo
        a.snd_pcm_hw_params_set_channels(h, hw, ch)
    a.snd_pcm_hw_params_set_rate(h, hw, 48000, 0)
    a.snd_pcm_hw_params_set_period_size(h, hw, 480, 0)
    a.snd_pcm_hw_params_set_periods(h, hw, 4, 0)
    if ck(a.snd_pcm_hw_params(h, hw), "hw_params %d" % dev) < 0:
        a.snd_pcm_close(h)
        return None
    sw = ctypes.c_void_p()
    a.snd_pcm_sw_params_malloc(ctypes.byref(sw))
    a.snd_pcm_sw_params_current(h, sw)
    boundary = ctypes.c_ulong()
    a.snd_pcm_sw_params_get_boundary(sw, ctypes.byref(boundary))
    a.snd_pcm_sw_params_set_start_threshold(h, sw, 480)
    a.snd_pcm_sw_params_set_stop_threshold(h, sw, boundary)  # never stop on xrun (HAL uses 0xFFFFFFFF)
    a.snd_pcm_sw_params(h, sw)
    a.snd_pcm_prepare(h)
    ck(a.snd_pcm_start(h), "start %d" % dev)                 # no data is ever written/read
    log("node %d up (%d ch)" % (dev, ch))
    return h


def pcm_close(h, dev):
    if h:
        a.snd_pcm_drop(h)
        a.snd_pcm_close(h)
        log("node %d closed" % dev)


# ---- the vendor mixer_paths.xml (+ the mixer_gains.xml it <include>s) -----------------------
# A path is DEFINED once as `<path name="X"> ...children... </path>`; elsewhere it appears as an
# empty reference `<path name="X"/>`. Keep only definitions (those with children), first wins, so a
# later reference never overwrites the real one. ElementTree does not follow <include>, so read both
# files. Top-level <ctl> of mixer_paths.xml are the route defaults audio_route resets to.
XML_GAINS = "/android/vendor/etc/mixer_gains.xml"
_paths = {}
_defaults = {}
for _f in (XML, XML_GAINS):
    try:
        _r = ET.parse(_f).getroot()
    except Exception as e:
        log("can't read %s: %s" % (_f, e))
        continue
    for _c in _r.iter("path"):
        if _c.get("name") and len(_c):
            _paths.setdefault(_c.get("name"), _c)
    if _f == XML:
        _defaults = {c.get("name"): (c.get("value") if c.get("value") is not None else c.get("id"))
                     for c in _r if c.tag == "ctl" and c.get("name")}


def expand(name, out, seen):
    p = _paths.get(name)
    if p is None:
        log("no such path: " + name)
        return
    if name in seen:
        return
    seen.add(name)
    for c in p:
        if c.tag == "path":
            expand(c.get("name"), out, seen)
        elif c.tag == "ctl":
            out.append((c.get("name"), c.get("value"), c.get("id")))


def controls_of(*names):
    out = []
    seen = set()
    for n in names:
        expand(n, out, seen)
    return out


# ---- the call-audio sequence ----------------------------------------------------------------
DEV = {"earpiece": ("handset", 0), "speaker": ("speaker", 4)}


def paths_for(device):
    dev = DEV[device][0]
    return ["incall_nb-%s" % dev, "gain-incall_nb-%s" % dev,
            "incall_nb-%s-mic" % dev, "gain-incall_nb-%s-mic" % dev]


def apply_media_route():
    """Put the normal speaker + mic route back, so PulseAudio plays again (proven-good files)."""
    try:
        data = b""
        for f in MEDIA_FILES:
            data += open(f, "rb").read() + b"\n"
        subprocess.run(["amixer", "-c0", "-q", "-s"], input=data, timeout=20)
        log("media route restored")
    except Exception as e:
        log("media restore failed: %s" % e)


class Call:
    def __init__(self):
        self.ctl = Ctl()
        self.device = "earpiece"
        self.mute = 0
        self.up = False
        self.nodes = {}
        self.applied = []

    def start(self):
        if self.up:
            return
        dev_name, param_dev = DEV[self.device]
        log("bringing up call audio on the %s" % self.device)
        self.ctl.set("ABOX Audio Mode", "IN_CALL")
        self.applied = paths_for(self.device)
        for name, value, elem_id in controls_of(*self.applied):
            if value is not None or elem_id is not None:
                self.ctl.set(name, value if value is not None else "0", elem_id)
        self.ctl.set("ABOX Call Path Param", param_dev)
        self.nodes[NODE_RX] = pcm_open_start(NODE_RX, STREAM_PLAYBACK, 2)
        self.nodes[NODE_TX] = pcm_open_start(NODE_TX, STREAM_CAPTURE, 4)
        self.nodes[NODE_REF] = pcm_open_start(NODE_REF, STREAM_CAPTURE, 2)
        self.ctl.set("ABOX Speech Volume", 7)
        self.ctl.set("ABOX DVTx Tx Mute", self.mute)
        self.up = True
        log("call audio up")

    def set_mute(self, m):
        self.mute = 1 if m else 0
        if self.up:
            self.ctl.set("ABOX DVTx Tx Mute", self.mute)

    def stop(self):
        if not self.up:
            return
        log("tearing call audio down")
        self.ctl.set("ABOX Out Fade start", 1)
        self.ctl.set("ABOX RDMA11 Func", "Mute")
        pcm_close(self.nodes.pop(NODE_RX, None), NODE_RX)
        self.ctl.set("ABOX RDMA11 Func", "Normal")
        pcm_close(self.nodes.pop(NODE_TX, None), NODE_TX)
        pcm_close(self.nodes.pop(NODE_REF, None), NODE_REF)
        self.ctl.set("ABOX CP call TX MUTE", 0)
        self.ctl.set("ABOX Translator Status", 0)
        self.ctl.set("ABOX Audio Mode", "NORMAL")
        self.ctl.set("ABOX Call Path Param", 0)
        for name, _value, _id in controls_of(*self.applied):     # incall controls back to defaults
            if name in _defaults:
                self.ctl.set(name, _defaults[name])
        self.ctl.set("ABOX DVTx Tx Mute", 0)
        self.up = False
        apply_media_route()
        log("call audio down")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] in ("parse", "dry"):
        device = sys.argv[2] if len(sys.argv) > 2 else "earpiece"
        if sys.argv[1] == "parse":
            for name, value, elem_id in controls_of(*paths_for(device)):
                print("%-34s = %s%s" % (name, value, (" [%s]" % elem_id) if elem_id else ""))
            return
        c = Call()
        c.device = device
        c.start()
        time.sleep(3)
        c.stop()
        return
    c = Call()
    for line in sys.stdin:
        parts = line.split()
        if not parts:
            continue
        cmd = parts[0]
        if cmd == "device" and len(parts) > 1 and parts[1] in DEV and not c.up:
            c.device = parts[1]
        elif cmd == "mute":
            c.set_mute(len(parts) > 1 and parts[1] == "1")
        elif cmd == "start":
            c.start()
        elif cmd == "stop":
            c.stop()
    c.stop()


if __name__ == "__main__":
    main()
