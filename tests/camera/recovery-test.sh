#!/bin/sh
# simulate a provider crash; the service must notice, redo the setup and come back by itself
unset LD_PRELOAD LD_LIBRARY_PATH
last=$(grep -E '\[pw\] .*: [a-z]+ -> [a-z]+' /var/log/s24-camera.log | tail -1)
case "$last" in *"-> streaming"*) echo "an app is streaming right now ($last); not testing"; exit 0;; esac
PP=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1); SP=$(pgrep -f '^/usr/local/sbin/s24-cam serve'|head -1)
echo "$(date +%T) killing provider $PP (s24-cam $SP)"; kill -9 $PP
t0=$(date +%s)
for i in $(seq 1 90); do
  NP=$(pgrep -f '^/usr/local/sbin/s24-cam serve'|head -1)
  [ -n "$NP" ] && [ "$NP" != "$SP" ] && grep -q 'a camera runs only while' /var/log/s24-camera.log.tail 2>/dev/null && break
  tail -5 /var/log/s24-camera.log > /var/log/s24-camera.log.tail; sleep 1
done
echo "$(date +%T) back after $(( $(date +%s) - t0 )) s: s24-cam $NP (was $SP), provider $(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1)"
rm -f /var/log/s24-camera.log.tail
grep -E 'died|exit|starting s24-cam|provider .* ready|fail' /var/log/s24-camera.log | tail -5 | cut -c1-140
sleep 2
su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_rear 3" 2>&1 | grep -E 'first frame|frames,'
