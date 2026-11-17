#!/bin/sh
# mobile data test (run in the pmOS root): everything forced onto rmnet1, never via Wi-Fi
IF=${1:-rmnet1}
echo "== radio:"; timeout 20 /usr/local/bin/s24-radio status 2>&1 | grep -E 'voice|data: (reg|not)|network: operator|signal' | sort -u
echo "== link:"; ip -br addr show dev $IF | sed -E 's/([0-9a-f]{4}:[0-9a-f]{4}:)[0-9a-f:]+/\1.../'
echo "== ping6 over $IF:"; ping -6 -c 3 -W 3 -I $IF 2001:4860:4860::8888 2>&1 | tail -2
echo "== DNS64 check via the carrier's DNS (ipv4only.arpa):"
nslookup -type=AAAA ipv4only.arpa 2a02:a47f:e000::53 2>&1 | grep -i -E 'address|can.t|timed' | grep -v '#53' | head -3
echo "== https over $IF (IPv6 only):"
curl -6 -sS --interface $IF -o /dev/null -m 20 -w 'http %{http_code}, %{size_download} bytes in %{time_total}s, %{speed_download} B/s\n' https://www.google.com/ 2>&1
echo "== 10 MB download over $IF:"
curl -6 -sS --interface $IF -o /dev/null -m 60 -w 'http %{http_code}, %{size_download} bytes in %{time_total}s, %{speed_download} B/s\n' https://speed.cloudflare.com/__down?bytes=10000000 2>&1
echo "== 5 MB upload over $IF:"
head -c 5000000 /dev/zero | curl -6 -sS --interface $IF -o /dev/null -m 60 -X POST --data-binary @- -w 'http %{http_code}, %{size_upload} bytes up in %{time_total}s, %{speed_upload} B/s\n' https://speed.cloudflare.com/__up 2>&1
