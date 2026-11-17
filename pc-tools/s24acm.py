#!/usr/bin/env python3
"""Talk to the S24 stage-0 serial shell (gadget ACM function) straight over libusb, for when Windows
has bound its ADB/WinUSB driver to the 18D1:D001 gadget instead of usbser (Claude, 2026-11-14).
  s24acm.py "CMD" [seconds]   send CMD + newline to the shell, print what comes back
"""
import sys
import time

import libusb_package
import usb.core
import usb.util

dev = libusb_package.find(idVendor=0x18D1, idProduct=0xD001)
if dev is None:
    sys.exit("no 18d1:d001 device")
cfg = dev.get_active_configuration()
print("interfaces:", [(i.bInterfaceNumber, hex(i.bInterfaceClass), hex(i.bInterfaceSubClass)) for i in cfg])
# ACM = the second CDC data interface (class 0x0A); the first belongs to NCM
data_itfs = [i for i in cfg if i.bInterfaceClass == 0x0A]
acm = data_itfs[-1]
for i in cfg:
    try:
        usb.util.claim_interface(dev, i.bInterfaceNumber)
    except usb.core.USBError as e:
        print("claim", i.bInterfaceNumber, "failed:", e)
ep_out = usb.util.find_descriptor(acm, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(acm, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
print("acm data interface", acm.bInterfaceNumber, "out", hex(ep_out.bEndpointAddress), "in", hex(ep_in.bEndpointAddress))
# SET_CONTROL_LINE_STATE (DTR|RTS) on the ACM control interface, as a terminal would
try:
    dev.ctrl_transfer(0x21, 0x22, 0x03, acm.bInterfaceNumber - 1, None)
except usb.core.USBError as e:
    print("line state:", e)
cmd = sys.argv[1] if len(sys.argv) > 1 else "echo SERIAL-OK"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 4
ep_out.write((cmd + "\n").encode())
out, end = b"", time.time() + secs
while time.time() < end:
    try:
        out += bytes(ep_in.read(ep_in.wMaxPacketSize * 64, 500))
    except usb.core.USBTimeoutError:
        pass
sys.stdout.write(out.decode(errors="replace"))
print()
