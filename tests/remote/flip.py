#!/usr/bin/env python3
# flip.py: a GTK 4 window that turns from black to white, or back, at each key press or click, and
# changes nothing otherwise: the time from a key in the browser to the changed picture there can be
# measured (latency.js) (Claude, 2026-11-20). Nested session only. Colours by CSS: the phone has no
# pycairo for a drawing area.
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk

CSS = b".s24-black { background: #000; } .s24-white { background: #fff; }"

def activate(app):
    provider = Gtk.CssProvider()
    provider.load_from_data(CSS, len(CSS))
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    w = Gtk.ApplicationWindow(application=app, title="s24rt flip")
    w.set_default_size(400, 300)
    area = Gtk.Box(hexpand=True, vexpand=True, focusable=True)
    area.add_css_class("s24-black")

    def flip(*_):
        white = area.has_css_class("s24-black")
        area.remove_css_class("s24-black" if white else "s24-white")
        area.add_css_class("s24-white" if white else "s24-black")

    def key(_c, _keyval, _code, _state):
        flip()
        return True

    keys = Gtk.EventControllerKey()
    keys.connect("key-pressed", key)
    w.add_controller(keys)
    click = Gtk.GestureClick()
    click.connect("pressed", flip)
    area.add_controller(click)
    w.set_child(area)
    w.present()
    area.grab_focus()

app = Gtk.Application(application_id="org.s24test.Flip")
app.connect("activate", activate)
app.run([__file__])
