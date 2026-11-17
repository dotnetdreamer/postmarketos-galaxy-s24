#!/bin/sh
unset LD_PRELOAD LD_LIBRARY_PATH
cpu(){ awk '/^cpu[0-9]/{print $1, $2+$3+$4+$6+$7+$8, $5}' /proc/stat; }
cpu > /tmp/c0; for p in /proc/[0-9]*; do echo "$(basename $p) $(awk '{print $14+$15, $39}' $p/stat 2>/dev/null) $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-70)"; done > /tmp/p0
sleep 5
cpu > /tmp/c1; for p in /proc/[0-9]*; do echo "$(basename $p) $(awk '{print $14+$15, $39}' $p/stat 2>/dev/null) $(tr '\0' ' ' < $p/cmdline 2>/dev/null | cut -c1-70)"; done > /tmp/p1
echo "== per-CPU busy % over 5 s:"
awk 'NR==FNR{b[$1]=$2;i[$1]=$3;next}{db=$2-b[$1]; di=$3-i[$1]; printf "%s %d%%  ", $1, (db+di)>0?100*db/(db+di):0}' /tmp/c0 /tmp/c1; echo
echo "== top processes over 5 s (ticks: 100 = one core fully busy for 1 s; last cpu):"
awk 'NR==FNR{t[$1]=$2;next}{d=$2-t[$1]; if(d>0){$2=d; print}}' /tmp/p0 /tmp/p1 | sort -k2 -rn | head -12
echo "== freqs (MHz): $(for c in 0 4 7 9; do echo -n "cpu$c=$(( $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq) / 1000 )) "; done)"
echo "== temps: $(for z in /sys/class/thermal/thermal_zone*; do printf "%s=%s " "$(cat $z/type)" "$(( $(cat $z/temp) / 1000 ))"; done)"
echo "== uclamp/affinity of camera procs:"; for P in $(pgrep -f '^/vendor/bin/hw/[^ ]*camera.provider-service_64') $(pgrep -f '^/usr/local/sbin/s24-cam serve'); do echo "   $P $(taskset -p $P | sed 's/.*: //') $(uclampset -p $P 2>/dev/null | tr '\n' ' ')"; done
