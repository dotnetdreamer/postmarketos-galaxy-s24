#!/bin/sh
# What NetworkManager and ModemManager's own tools see with s24-mmd; traffic forced onto the mobile
# link. Identifiers (IMEI, IMSI, ICCID) masked to their first 5 digits.
mask() { sed -E 's/([0-9]{5})[0-9]{6,}/\1xxxxxxxxxx/g; s/(2a02:[0-9a-f]+:)[0-9a-f:]+/\1.../g'; }
echo "== mmcli -L:"; mmcli -L 2>&1 | mask
echo "== mmcli -m 0 (status part):"
mmcli -m 0 2>&1 | grep -E 'manufacturer|model|revision|state|power state|access tech|signal quality|imei|operator|registration|packet|Bearer|SIM  |primary port|ports' | mask
echo "== mmcli bearer:"; mmcli -b 0 2>&1 | grep -E 'connected|interface|apn|method|address|prefix|gateway|mtu' | mask
echo "== nmcli:"; nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device | grep -i -E 'gsm|s24|rmnet'
nmcli -t -f GENERAL.STATE,GENERAL.IP-IFACE device show s24-ril 2>&1 | head -3
echo "== rmnet0:"; ip -6 -br addr show dev rmnet0 2>&1 | mask
echo "== rules:"; ip rule | grep 1430; ip -6 rule | grep 1430
echo "== table 1430 (v6):"; ip -6 route show table 1430 2>&1 | mask | head -4
echo "== resolv.conf:"; head -n3 /etc/resolv.conf | mask
echo "== clatd: $(pgrep -x clatd >/dev/null && echo running || echo no) $(ip -4 route show default | grep clat)"
echo "== IPv6 over rmnet0: $(curl -6 -sS --interface rmnet0 -o /dev/null -m 15 -w 'http %{http_code} %{time_total}s' https://www.google.com/ 2>&1)"
echo "== IPv4 via clat: $(curl -4 -sS --interface clat -o /dev/null -m 15 -w 'http %{http_code} %{time_total}s' https://1.1.1.1/ 2>&1)"
echo "== tail of the s24-mmd log:"; tail -n 25 /var/log/s24-mmd.log | mask
