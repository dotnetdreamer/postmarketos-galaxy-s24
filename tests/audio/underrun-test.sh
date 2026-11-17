#!/bin/sh
# Underrun test (Claude, 2026-11-15): zeros through PulseAudio (inaudible) for SECS seconds while N
# busy loops load the CPUs; counts what PulseAudio reports. LAT = client latency (ms) a player asks
# for (empty = pacat's default 2 s: only the sink side can underrun). PA's own log, followed live.
N=${1:-10}; SECS=${2:-20}; LAT=$3; CHRT=${4:+chrt -r $4}
U=user; export XDG_RUNTIME_DIR=/run/user/$(id -u $U)
asu() { su $U -s /bin/sh -c "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR $*"; }
out=/tmp/pa-underrun-test.log
asu "pacmd set-log-level 4" >/dev/null 2>&1
logread -f -u pulseaudio > $out 2>&1 &
lr=$!
sleep 1
$CHRT su $U -s /bin/sh -c "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR timeout $SECS pacat --playback --format=s16le --rate=48000 --channels=2 ${LAT:+--latency-msec=$LAT} < /dev/zero" &
sleep 2
pids=""
i=0; while [ $i -lt $N ]; do (while :; do :; done) & pids="$pids $!"; i=$((i + 1)); done
sleep $((SECS - 3))
[ -n "$pids" ] && kill $pids 2>/dev/null
sleep 2
asu "pacmd set-log-level 2" >/dev/null 2>&1
sleep 1
kill $lr 2>/dev/null
echo "load=$N secs=$SECS client_latency=${LAT:-default}: sink 'Underrun!'=$(grep -c 'Underrun!' $out) 'Buffer underrun'=$(grep -c 'Buffer underrun' $out) client 'end of underrun'=$(grep -c 'end of underrun' $out)"
grep -E 'latency|Requested|tlength|buffer metrics' $out | head -4 | cut -c1-200
