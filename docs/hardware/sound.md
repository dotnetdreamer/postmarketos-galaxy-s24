# Sound

**State:** speaker and main microphone work through PulseAudio. Call audio is separate, see [modem.md](modem.md).

## How the audio path works

```
app -> PulseAudio -> ALSA hw:0,100 (calliope_0, a virtual PCM) -> ABOX DSP (Samsung "Calliope" firmware)
    -> mixer -> UAIF3 -> 2x Cirrus cs35l43 amplifiers (I2C 2-0040/2-0041) -> speakers
microphone: ALSA hw:0,120 (calliope_20)
```

## Setup (files in rootfs/)

- `/usr/local/share/s24/s24-speaker.amixer`, `s24-mic.amixer`: the mixer settings, generated from Samsung's `mixer_paths.xml` (the `media-speaker` and `recording-mic` paths, with the plain VDSP route instead of Samsung's SoundBooster)
- `/etc/local.d/s24-audio.start`: waits for the ABOX DSP, applies both, and restores the patched PulseAudio module after a same-version package reinstall (log: `/var/log/s24-audio.log`)
- `/etc/pulse/default.pa.d/s24-sound.pa`: loads the two PCMs with 10 ms fragments, `tsched=0`, `mmap=false`, `rewind_safeguard` = the whole buffer
- `/etc/udev/rules.d/90-s24-sound.rules`: hides the card from PulseAudio's auto-detection
- The user must be in group `audio`

## The two DSP rules

1. **Periods must be 20 ms or shorter**, or the firmware never schedules the stream ("period is over PERIOD_BOUND, NO SCHEDULED" in the DSP log). Use 10 ms
2. **Every write is booked as one whole period.** PulseAudio writes partial periods when resampling, the DSP double-counts, overflows and mutes: stutter. `src/audio/pa-whole-periods.patch` makes PulseAudio's ALSA sink write whole periods only

Rebuild the patched module on the phone, as root (about 2 minutes, only `libalsa-util.so` is built; it must match the installed `pulseaudio-alsa` version, 17.0-r9 here):

```sh
sudo sh src/audio/pa-fetch.sh && sudo sh src/audio/pa-build.sh && sudo sh src/audio/pa-install.sh
```

The build packages come in as the apk virtual package `.s24-pa-build`; `apk del .s24-pa-build` removes them. After a PulseAudio upgrade, rebuild (the boot script warns in its log when the version changed).

## Debugging

- `/proc/abox/log-00`: the DSP's own log (it blocks: `cat` it in the background, kill it after)
- `tests/audio/abox-trace.sh`: kprobes on the driver's IPC functions (message types to and from the DSP)
- `tests/audio/silent-tone-check.sh`: plays a tone with the amps muted and checks the amp DSP's "Protection AUDIO_IN_MAX" control, a full-path test that makes no noise
- `tests/audio/ack-analyze.sh`: counts writes that don't line up with a period

No speaker calibration exists on this unit (`/efs/cirrus` missing); the amps' protection firmware runs anyway.

## Sound on a USB-C monitor

The DisplayPort PCM (`hw:0,31`) and the sink `s24-dockd` adds for it: see [usb-c-display.md](usb-c-display.md#sound-on-the-monitor).
