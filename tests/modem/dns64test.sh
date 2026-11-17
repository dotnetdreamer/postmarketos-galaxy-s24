#!/bin/sh
# Does the carrier's mobile DNS synthesize IPv6 for IPv4-only names (DNS64), and does NAT64 carry them?
# Queries pinned to rmnet0 with temporary host routes (removed at the end).
IF=${1:-rmnet0}; D1=2a02:a47f:e000::53; D2=2a02:a47f:e000::54
ip -6 route add $D1/128 dev $IF 2>/dev/null; ip -6 route add $D2/128 dev $IF 2>/dev/null
for h in ipv4only.arpa github.com; do
  echo "== AAAA $h via the mobile DNS:"
  nslookup -type=AAAA $h $D1 2>&1 | sed -n '/^Name:/,$p' | grep -i -E 'address' | head -2
done
A=$(nslookup -type=AAAA github.com $D1 2>/dev/null | sed -n '/^Name:/,$p' | awk '/Address/ {print $NF; exit}' | sed 's/^Address: *//')
echo "== https to github.com via its synthesized address [$A] over $IF:"
[ -n "$A" ] && curl -6 -sS --interface $IF -o /dev/null -m 20 --resolve github.com:443:[$A] -w 'http %{http_code} in %{time_total}s\n' https://github.com/ 2>&1
ip -6 route del $D1/128 dev $IF 2>/dev/null; ip -6 route del $D2/128 dev $IF 2>/dev/null
