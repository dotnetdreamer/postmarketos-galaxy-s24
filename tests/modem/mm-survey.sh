#!/bin/sh
# what a ModemManager-compatible bridge can build on, and who on this phone talks to ModemManager
echo "== versions: $(apk info -e -v modemmanager networkmanager libmm-glib 2>/dev/null | tr '\n' ' ')"
echo "== mm dev headers available: $(apk search -e libmm-glib-dev modemmanager-dev 2>/dev/null | tr '\n' ' ')"
echo "== gdbus-codegen: $(command -v gdbus-codegen)  MM interface XMLs: $(ls /usr/share/dbus-1/interfaces/ 2>/dev/null | grep -c ModemManager1)"
echo "== MM typelib: $(ls /usr/lib/girepository-1.0/ 2>/dev/null | grep -i modemmanager)"
echo "== NM wwan plugin: $(find /usr/lib/NetworkManager -name '*wwan*' 2>/dev/null)"
echo "== real ModemManager: $(pgrep -a ModemManager) | rc: $(rc-status -a 2>/dev/null | grep -i modemmanager | tr -s ' ')"
ls /usr/share/dbus-1/system-services/ | grep -i modem
