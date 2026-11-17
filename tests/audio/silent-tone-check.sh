#!/bin/sh
# Silent end-to-end check (Claude, 2026-11-15): both speaker amps muted, a -20 dBFS 440 Hz tone at
# 44.1 kHz (PulseAudio resamples it: the case that stuttered) through PulseAudio; the amps' own
# input meters (DSP1 Protection AUDIO_IN_MAX) show what reached them. The mutes are put back as
# they were whatever happens.
A=/usr/local/bin/asuser
L0=$(amixer -c0 cget name='Left AMP Mute' | sed -n 's/.*: values=//p')
R0=$(amixer -c0 cget name='Right AMP Mute' | sed -n 's/.*: values=//p')
restore() {
	amixer -c0 -q cset name='Left AMP Mute' "$L0"
	amixer -c0 -q cset name='Right AMP Mute' "$R0"
}
trap restore EXIT INT TERM
amixer -c0 -q cset name='Left AMP Mute' 0
amixer -c0 -q cset name='Right AMP Mute' 0
meter() {
	for s in Left Right; do
		printf '%s=%s ' $s "$(amixer -c0 cget name="$s DSP1 Protection 5f210 AUDIO_IN_MAX" | sed -n 's/.*: values=//p')"
	done
}
python3 -c "
import math, struct
a = 32767 * 10 ** (-20 / 20); n = 44100 * 4; f = 2205
with open('/tmp/tone441.raw', 'wb') as o:
    o.write(b''.join(struct.pack('<hh', *(2 * [int(a * min(1, i / f, (n - i) / f) * math.sin(2 * math.pi * 440 * i / 44100))])) for i in range(n)))
"
echo "mutes now: L=$(amixer -c0 cget name='Left AMP Mute' | sed -n 's/.*: values=//p') R=$(amixer -c0 cget name='Right AMP Mute' | sed -n 's/.*: values=//p') (0 = Muted)"
echo "meters before: $(meter)"
D=/tmp/tone-dsp.log
timeout 2 cat /proc/abox/log-00 > /dev/null 2>&1
cat /proc/abox/log-00 > $D 2>/dev/null &
rd=$!
$A -- sh -c 'pacat --playback --format=s16le --rate=44100 --channels=2 < /tmp/tone441.raw' &
sleep 2
echo "meters while the tone plays: $(meter)"
wait $!
sleep 1
kill $rd 2>/dev/null
echo "DSP overflows during the tone: $(grep -c 'overflow detected' $D)"
restore
trap - EXIT INT TERM
echo "mutes restored: L=$(amixer -c0 cget name='Left AMP Mute' | sed -n 's/.*: values=//p') R=$(amixer -c0 cget name='Right AMP Mute' | sed -n 's/.*: values=//p') (was L=$L0 R=$R0)"
rm -f /tmp/tone441.raw
