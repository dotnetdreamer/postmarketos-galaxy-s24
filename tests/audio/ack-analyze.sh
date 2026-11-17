#!/bin/sh
# ACK offsets in a DSP log: whole 10 ms blocks are multiples of 1920 bytes (S16 stereo, 480 frames)
f=${1:-/tmp/chromium-dsp.log}
grep 'VPCM ACK' "$f" | sed -E 's/.*VPCM ACK: [0-9]+, 0x([0-9A-Fa-f]+).*/\1/' |
	while read -r h; do echo $((0x$h)); done > /tmp/acks.txt
echo "ACKs=$(wc -l < /tmp/acks.txt) whole-block offsets=$(awk '$1 % 1920 == 0' /tmp/acks.txt | wc -l) mid-block offsets=$(awk '$1 % 1920 != 0' /tmp/acks.txt | wc -l)"
echo "most common offsets: $(sort -n /tmp/acks.txt | uniq -c | sort -rn | head -8 | tr -s ' ' | tr '\n' ';')"
