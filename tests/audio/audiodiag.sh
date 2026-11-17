#!/bin/sh
# what plays where, buffer settings, underruns, CPU (run in the pmOS root as root)
U=$(id -u user); export XDG_RUNTIME_DIR=/run/user/$U
pa() { su user -s /bin/sh -c "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR pactl $*" 2>&1; }
echo "== sinks:"; pa list sinks short
echo "== streams:"; pa list sink-inputs | grep -E 'Sink Input #|Sink:|application.name =|Latency|Buffer|Corked' | sed 's/^\s*//'
for s in /proc/asound/card*/pcm*p/sub0; do
  st=$(cat $s/status 2>/dev/null | head -1)
  case "$st" in *RUNNING*|*XRUN*|*DRAINING*) echo "== $s: $st"; cat $s/hw_params; grep -E 'avail_max|appl_ptr|hw_ptr' $s/status ;; esac
done
echo "== PA process:"; ps -o pid,user,nice,rtprio,args 2>/dev/null | grep -E '[p]ulseaudio' | head -3
for p in $(pgrep -x pulseaudio); do echo "pid $p: $(grep -E '^(voluntary|nonvoluntary)' /proc/$p/status | tr '\n' ' ') sched=$(awk '{print "policy",$41,"prio",$18,"nice",$19}' /proc/$p/stat)"; done
echo "== CPU load (top 6):"; top -b -n 1 | sed -n '1,4p;8,14p' | cut -c1-110
echo "== cpufreq:"; for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$(basename $p): $(cat $p/scaling_governor) cur $(cat $p/scaling_cur_freq) max $(cat $p/scaling_max_freq)/$(cat $p/cpuinfo_max_freq)"; done
