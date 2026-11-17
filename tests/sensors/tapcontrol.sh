#!/bin/sh
# control: the same injected double tap with the old service, then with the fixed one
S=/newroot/usr/local/sbin
cp $S/s24-powerd /newroot/root/s24-powerd.fixed
cp $S/s24-powerd.before-kwin-fix $S/s24-powerd
chroot /newroot rc-service s24-power restart >/dev/null 2>&1
sleep 2
echo "== OLD service: $(md5sum $S/s24-powerd | cut -c1-8)"
chroot /newroot python3 /root/taptest.py off
sleep 4
chroot /newroot python3 /root/taptest.py tap
sleep 4
cp /newroot/root/s24-powerd.fixed $S/s24-powerd
chroot /newroot rc-service s24-power restart >/dev/null 2>&1
sleep 3
echo "== FIXED service: $(md5sum $S/s24-powerd | cut -c1-8)"
chroot /newroot python3 /root/taptest.py tap
echo "== log:"
tail -12 /newroot/var/log/s24-power.log
