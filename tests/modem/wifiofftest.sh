#!/bin/sh
# Wi-Fi off for ~40 s: do apps work on mobile data alone? Wi-Fi is switched back on whatever happens.
trap 'nmcli radio wifi on' EXIT INT TERM
show() { echo "== $*"; }
show "before: $(head -n1 /etc/resolv.conf)"
nmcli radio wifi off
sleep 6
show "resolv.conf with Wi-Fi off:"; cat /etc/resolv.conf | sed -E 's/(2a02:[0-9a-f]+:)[0-9a-f:]+/\1.../g'
show "default routes:"; ip -4 route show default; ip -6 route show default | sed -E 's/(2a02:[0-9a-f]+:)[0-9a-f:]+/\1.../g'
show "name lookup (system resolver):"; getent hosts github.com | head -1
show "IPv4-only site by name (DNS64/NAT64), no interface forced:"
curl -sS -o /dev/null -m 20 -w 'github.com: http %{http_code}, %{remote_ip}, %{time_total}s\n' https://github.com/ 2>&1
show "IPv4 literal (CLAT), no interface forced:"
curl -sS -o /dev/null -m 20 -w '1.1.1.1: http %{http_code}, %{time_total}s\n' https://1.1.1.1/ 2>&1
show "IPv6 site:"; curl -sS -o /dev/null -m 20 -w 'google.com: http %{http_code}, %{remote_ip}, %{time_total}s\n' https://www.google.com/ 2>&1
show "10 MB by name:"; curl -sS -o /dev/null -m 60 -w 'http %{http_code}, %{speed_download} B/s\n' 'https://speed.cloudflare.com/__down?bytes=10000000' 2>&1
nmcli radio wifi on
trap - EXIT
for i in $(seq 1 20); do nmcli -t -f STATE general | grep -q '^connected' && break; sleep 1; done
sleep 3
show "after Wi-Fi is back: $(nmcli -t -f STATE general), $(head -n1 /etc/resolv.conf)"
ip -4 route show default
