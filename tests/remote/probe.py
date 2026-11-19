#!/usr/bin/env python3
# probe.py LOG: a GTK 4 window that writes down every key, click, scroll and the text typed into it
# (Claude, 2026-11-19). Runs only inside the private nested session.
import sys, time
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk

LOG = sys.argv[1]


def log(*a):
    with open(LOG, "a") as f:
        f.write("%.3f %s\n" % (time.time(), " ".join(str(x) for x in a)))


def on_key(c, kv, code, st):
    log("key", Gdk.keyval_name(kv), "code", code, "state", int(st))
    return False


def on_scroll(c, dx, dy):
    log("scroll", dx, dy)
    return False


def activate(app):
    w = Gtk.ApplicationWindow(application=app, title="s24rt probe")
    w.set_default_size(800, 480)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
    btn = Gtk.Button(label="Press me")
    btn.connect("clicked", lambda *_: log("button clicked"))
    view = Gtk.TextView()
    view.set_vexpand(True)
    buf = view.get_buffer()
    buf.connect("changed", lambda b: log("text", repr(b.get_text(b.get_start_iter(), b.get_end_iter(), True))))
    box.append(btn)
    box.append(view)
    w.set_child(box)
    keys = Gtk.EventControllerKey()
    keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
    keys.connect("key-pressed", on_key)
    w.add_controller(keys)
    click = Gtk.GestureClick(button=0)
    click.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
    click.connect("pressed", lambda g, n, x, y: log("press", g.get_current_button(), "n", n, "at", round(x), round(y)))
    w.add_controller(click)
    scroll = Gtk.EventControllerScroll(flags=Gtk.EventControllerScrollFlags.BOTH_AXES)
    scroll.connect("scroll", on_scroll)
    w.add_controller(scroll)
    w.present()
    view.grab_focus()
    log("probe up")


app = Gtk.Application(application_id="org.s24test.ProbeWindow")
app.connect("activate", activate)
app.run([sys.argv[0]])
