#!/bin/sh
# Headless Chromium plays an inaudible file (+-1 LSB noise) under CPU load; counts PulseAudio's
# underrun reports for its stream. $1 = extra flags (e.g. --audio-buffer-size=512 = the old way).
EXTRA=$1; N=${2:-10}; SECS=${3:-25}
A=/usr/local/bin/asuser
T=/tmp/s24-chromium-audiotest; cp $D /tmp/chromium-dsp.log 2>/dev/null; rm -rf $T; mkdir -p $T; chown user $T
python3 - << 'PY'
import struct, random, wave
w = wave.open('/tmp/s24-chromium-audiotest/quiet.wav', 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
r = random.Random(1)
w.writeframes(b''.join(struct.pack('<hh', r.choice((-1, 0, 1)), r.choice((-1, 0, 1))) for _ in range(48000 * 40)))
w.close()
PY
cat > $T/play.html << 'H'
<audio src="quiet.wav" autoplay loop></audio>
H
chown -R user $T
out=$T/pa.log
$A -- pacmd set-log-level 4 >/dev/null 2>&1
logread -f -u pulseaudio > $out 2>&1 &
lr=$!
D=$T/dsp.log
timeout 2 cat /proc/abox/log-00 > /dev/null 2>&1
cat /proc/abox/log-00 > $D 2>/dev/null &
rd=$!
$A -- chromium --user-data-dir=$T/profile --no-first-run --no-default-browser-check \
	--autoplay-policy=no-user-gesture-required $EXTRA --app=file://$T/play.html > $T/chromium.out 2>&1 &
sleep 8
logger -t pulseaudio "S24TEST phase: playing, load starts"
echo "== stream:"; $A -- pactl list sink-inputs | grep -E 'application.name =|Buffer Latency|Sink Latency' | sed 's/^\s*//'
pids=""; i=0; while [ $i -lt $N ]; do (while :; do :; done) & pids="$pids $!"; i=$((i + 1)); done
sleep $SECS
logger -t pulseaudio "S24TEST phase: load stops"
kill $pids 2>/dev/null
sleep 3
logger -t pulseaudio "S24TEST phase: closing chromium"
pkill -u user -f s24-chromium-audiotest/profile 2>/dev/null
sleep 2
$A -- pacmd set-log-level 2 >/dev/null 2>&1
kill $lr $rd 2>/dev/null
echo "DSP: overflows=$(grep -c 'overflow detected' $D) bursts=$(grep 'VPCM ACK' $D | grep -c 'P: +000\.') | PA: rewinds=$(grep -c 'Requested to rewind' $out) by size: $(grep -o 'Requested to rewind [0-9]* bytes' $out | sort | uniq -c | tr -s ' ' | tr '
' ';')"
echo "flags='${EXTRA:-(defaults from /etc/chromium)}' load=$N for ${SECS}s: client underruns=$(grep -c 'end of underrun' $out) sink underruns=$(grep -c -E 'Underrun!|Buffer underrun' $out)"
echo "== timeline:"; grep -E "S24TEST|end of underrun|Created input|Freeing input" $out | awk '{t=$3; sub(/^.*\]: /, ""); print t, substr($0, 1, 110)}'
echo "== chromium said:"; grep -v -E "^s*$" $T/chromium.out | tail -6 | cut -c1-180; cp $D /tmp/chromium-dsp.log 2>/dev/null; rm -rf $T
