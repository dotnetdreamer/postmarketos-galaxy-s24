#!/usr/bin/env python3
# anim.py: a GTK 4 window that changes every frame (a counter and a moving bar), so the screen
# always has something new to send (Claude, 2026-11-19). Nested session only.
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib

def activate(app):
    w = Gtk.ApplicationWindow(application=app, title="s24rt anim")
    w.set_default_size(500, 200)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    label = Gtk.Label()
    bar = Gtk.LevelBar(min_value=0, max_value=100)
    box.append(label)
    box.append(bar)
    w.set_child(box)
    n = [0]
    def tick(widget, clock):
        n[0] += 1
        label.set_markup("<span size='40000'>%06d</span>" % n[0])
        bar.set_value(n[0] % 100)
        return True
    w.add_tick_callback(tick)
    w.present()

app = Gtk.Application(application_id="org.s24test.Anim")
app.connect("activate", activate)
app.run([__file__])
