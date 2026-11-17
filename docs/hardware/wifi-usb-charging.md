# Wi-Fi, USB and charging

## Wi-Fi

**State:** works. NetworkManager manages `wlan0`; it reconnects after reboot and can wake the phone from deep sleep on incoming packets.

- Chip: Qualcomm "kiwi" (WCN785x) on PCIe/MHI
- Stage 0 loads it (`stage0/init`): bind `/newroot/vendor` to `/vendor` (firmware path), `insmod` `rfkill`, `cfg80211`, `kiwi_v2` from `/newroot/lib/modules/{system_dlkm,vendor_dlkm}`, then `echo 1 > /sys/devices/platform/qcom,cnss-kiwi/fs_ready`. Cold calibration takes about 25 s, then `wlan0` appears
- Firmware: `/vendor/firmware/kiwi/` and `/vendor/etc/wifi/kiwi_v2/qcom_cfg.ini` from your own vendor image
- The MAC address is random on each boot (the real one is in EFS; not wired up)

## USB

- Stage 0 sets up a configfs gadget with NCM (network) and ACM (serial) functions as `1d6b:0104`: phone `172.16.42.1`, PC `172.16.42.2` by DHCP, root telnet on the cable only
- `rootfs/etc/NetworkManager/conf.d/50-s24-usb0.conf` keeps NetworkManager away from `usb0`
- The serial function is a second rescue path when the network doesn't come up (`pc-tools/s24acm.py` talks to it straight over libusb)
- A modem power change resets the USB link once (TCP over the cable drops)
- Monitors, docks and USB devices on the phone (USB host mode): see [usb-c-display.md](usb-c-display.md)

## Charging

**State:** works, from chargers and from a PC's USB port.

Samsung's battery driver caps a PC's port at 100 mA until Samsung's USB stack reports the gadget configured. Stage 0 does that report itself: when `/sys/class/udc/*/state` becomes `configured` it writes `850` (SuperSpeed) or `475` (high speed) to `/sys/class/power_supply/battery/usb_conf_test`. Chargers that aren't computers are handled by the driver itself.

With the USB data cable connected to a PC the phone never deep-sleeps (USB wakeup sources stay held, like on Android). On a plain charger it sleeps normally.
