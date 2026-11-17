#!/bin/sh
# sustained camera run: CPU per process, clocks, temperatures, throttling, battery current, fps
#   thermtest.sh MODE SECONDS     MODE = floor (uclamp min 512, as installed) | nofloor (min 0)
exec > /tmp/thermtest-$1.log 2>&1
unset LD_PRELOAD LD_LIBRARY_PATH
MODE=${1:-floor}; DUR=${2:-180}
echo s24-thermtest > /sys/power/wake_lock
PP=$(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64'|head -1)
SP=$(pgrep -f '^/usr/local/sbin/s24-cam serve'|head -1)
CD=$(pgrep -f '^/usr/local/sbin/s24-camd'|head -1)
KW=$(pgrep -x kwin_wayland|head -1)
M=512; [ "$MODE" = nofloor ] && M=0
uclampset -a -m $M -p $PP >/dev/null 2>&1; uclampset -a -m $M -p $SP >/dev/null 2>&1
temps(){ for z in /sys/class/thermal/thermal_zone*; do printf "%s=%s " "$(cat $z/type)" "$(( $(cat $z/temp) / 1000 ))"; done; }
thr(){ for c in /sys/class/thermal/cooling_device*; do s=$(cat $c/cur_state); [ "$s" != 0 ] && printf "%s=%s " "$(cat $c/type | sed 's/thermal-//')" $s; done; }
frq(){ for c in 0 4 7 9; do printf "cpu%s=%s " $c $(( $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq) / 1000 )); done; }
bat(){ printf "battery %s%% %smA" "$(cat /sys/class/power_supply/battery/capacity)" "$(cat /sys/class/power_supply/battery/current_now)"; }
tk(){ awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0; }
total(){ awk '/^cpu /{print $2+$3+$4+$6+$7+$8, $5}' /proc/stat; }
echo "$(date +%T) mode $MODE (clock floor $M), $DUR s; provider $PP s24-cam $SP s24-camd $CD"
echo "idle:  $(temps)| $(frq)| $(bat)"
n0=$(grep -c '\[stream\]' /var/log/s24-camera.log)
su user -c "XDG_RUNTIME_DIR=/run/user/10000 /home/user/pwlat samsung_rear $DUR" > /tmp/thermtest-pwlat.log 2>&1 &
sleep 3
LP=$(pgrep -f '^/home/user/pwlat'|head -1)
a1=$(tk $PP); a2=$(tk $SP); a3=$(tk $CD); a4=$(tk $LP); a5=$(tk $KW); set -- $(total); T0=$1; I0=$2
t=0
while [ $t -lt $((DUR - 10)) ]; do
	sleep 15; t=$((t + 15))
	echo "+${t}s: $(temps)| $(frq)| $(bat)| throttled: $(thr)"
done
set -- $(total); T1=$1; I1=$2
S=$((t + 3))
echo "CPU over ${t} s (100% = one core): provider $(( ($(tk $PP) - a1) / t ))%  s24-cam $(( ($(tk $SP) - a2) / t ))%  s24-camd $(( ($(tk $CD) - a3) / t ))%  test app $(( ($(tk $LP) - a4) / t ))%  kwin $(( ($(tk $KW) - a5) / t ))%  | whole phone $(( 100 * (T1 - T0) / (T1 - T0 + I1 - I0) ))% of 10 cores"
wait
grep -E 'first frame|frames,' /tmp/thermtest-pwlat.log
sed -n "$((n0 + 1)),\$p" /var/log/s24-camera.log | grep '\[stream\]' | cut -c1-140
uclampset -a -m 512 -p $PP >/dev/null 2>&1; uclampset -a -m 512 -p $SP >/dev/null 2>&1
echo "$(date +%T) done (clock floor back to 512)"
echo s24-thermtest > /sys/power/wake_unlock
