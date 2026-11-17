#!/bin/sh
# NetworkManager switches the way the desktop uses them; log /tmp/nm-toggle.out
exec > /tmp/nm-toggle.out 2>&1
st() { echo "  $(date '+%T') nm: $(nmcli -t -f DEVICE,STATE device | grep '^s24-ril' | cut -d: -f2) | mm: $(mmcli -m 0 2>/dev/null | grep -E '^\s+\|\s+state:' | sed 's/.*state: //;s/\x1b\[[0-9;]*m//g')"; }
waitfor() { i=0; while [ $i -lt $2 ]; do nmcli -t -f DEVICE,STATE device | grep -q "^s24-ril:$1\$" && break; sleep 1; i=$((i + 1)); done; echo "  -> $1 after ${i}s"; }
echo "== resolv.conf after the Wi-Fi test: $(head -n1 /etc/resolv.conf)"
st
echo "== connection down"; nmcli connection down "Mobile data" >/dev/null; waitfor disconnected 30; st
echo "== connection up"; nmcli connection up "Mobile data" >/dev/null; waitfor connected 60; st
echo "== mobile radio off (wwan off)"; nmcli radio wwan off; waitfor unavailable 40; st
echo "== mobile radio on (wwan on)"; nmcli radio wwan on; waitfor connected 120; st
echo "== traffic: $(curl -6 -sS --interface rmnet0 -o /dev/null -m 15 -w 'http %{http_code} %{time_total}s' https://www.google.com/ 2>&1)"
echo "== DONE"
