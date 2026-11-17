# Galaxy S24 (s5e9945): what Samsung's audio HAL does for a CP (circuit-switched) voice call

Static analysis, 2026-11-16, of the stock vendor libraries pulled to this folder:
`audio.primary.s5e9945.so` (the HAL), `libaudioproxy2.so` (ABOX proxy: routes and PCM nodes),
`libaudio-ril.so` (the `SecRil*` API), `libsecril-client.so` (the socket client to rild),
`mixer_paths.xml` (which `<include>`s `mixer_gains.xml`). Nothing on the phone was changed.
The only phone access was a read-only `amixer -c0 cget` of the controls named here (all exist)
and a grep of `vendor/build.prop`.

Tools: `fn.py LIB SYMBOL|0xADDR [bytes]` (llvm-objdump from the NDK plus string/GOT
annotation), `xp.py PATH...` (expand a mixer path with its includes), `flat.py PATH...`
(effective control list plus the XML reset values, used for the appendix).

Marked **UNCONFIRMED** = not proven by the code read so far; the reason is given.

---

## 0. Names and numbers used below

| thing | value |
|---|---|
| HAL device ids (proxy `device_table`) | 0 = `handset`, 1 = `speaker`, 2 = `headset`, 3 = `headphone`, 200 = `none`; >= 100 = capture devices (`...-mic`, `...-in`) |
| route path name | `<usage>-<device>`, e.g. `incall_nb-handset` (playback) and `incall_nb-handset-mic` (capture) |
| gain path name | `"gain-" + route path` (proxy base-class method, vtable slot +0x118), e.g. `gain-incall_nb-handset` |
| CP-call usage | ausage 20 = `incall_nb` (default). The band map `dev_map_samplingrate_voice` can pick another usage (e.g. `incall_wb`), but **`incall_wb-*` and `gain-incall_wb-*` are byte-identical to `incall_nb-*`** (checked: handset, handset-mic, speaker, speaker-mic). So NB vs WB only changes the "Call Path Param" band bits. |
| loop PCM nodes | card 0: **104 "Voice Call Rx"** (playback), **123 "Voice Call Tx"** (capture), **131 "Voice Call Ref Tx"** (capture, echo reference) |
| call volume steps | `ro.config.vc_call_vol_steps=8` on this phone (code default 5) |

The audio itself does not pass through these PCMs: the paths route CP to ABOX inside the DSP
(`ABOX SPUS ASRC11 IS = CP` for downlink, `ABOX SPUM ASRC6 OS = CP` / `ASRC0 OS = CP` for uplink
and reference). The PCM nodes only have to be open and started so the DMA/ASRC engines run.

---

## 1. CALL START, in order

Trigger on stock: Telecom sets mode IN_CALL, then AudioPolicy routes the primary output to the
earpiece (or speaker/headset) while in call mode.

### 1a. `adev_set_mode(IN_CALL=2)`
1. Adapter pre-check (`checkConditionForAudioMode`), no hardware effect found.
2. `proxy_set_audiomode(proxy, 2)`:
   - mixer **`ABOX Audio Mode` = `IN_CALL` (2)** (enum: NORMAL, RINGTONE, IN_CALL, IN_COMMUNICATION, IN_VIDEOCALL, RESERVED0, RESERVED1, IN_LOOPBACK).
   - (Only when the new mode is NORMAL: clear Call Path Param, see 2a.)
3. `s_checkPostConditionForAudioMode(adev, 2)`:
   - `voice_set_ipc_cmd(voice, 1, 2)` → `SecRilSetIpcCmd(1, 2)` → libaudio-ril `setOemIpcCommand(1, mode)` → `SendOemIpcCommand(client, 9, 0x0f, 3, &mode, 1)`. **Modem message, sent on every mode change** (mode >= 4 is sent as 0xfe). Bytes in section 6.
   - `voice_set_call_mode(voice, true)`: HAL state 0 → 1 (no I/O).
   - `proxy_call_status(proxy, 1)`: stores a flag and notifies the sound-trigger HAL only (no mixer, no modem).

### 1b. Routing the primary output in call mode (`AudioHalOdmAdapterStream::doStreamRoutingForPlayback`)
1. `adev_set_route_locksRequired(out, 0, set=true, force)` → `set_route_for_playback` / `set_route_for_capture` → proxy `set_route` for the call usage:
   - playback: `audio_route_apply_path("incall_nb-<dev>")`, then `audio_route_apply_path("gain-incall_nb-<dev>")`; the previous path is reset first (`audio_route_reset_path`), then `audio_route_update_mixer`.
   - capture: same with `incall_nb-<dev>-mic` and `gain-incall_nb-<dev>-mic` (earpiece and speaker calls: `incall_nb-handset-mic` / `incall_nb-speaker-mic`).
   - USB-only extras (`set-callref-wdma-mono-channel`, `set-usbout-dualloop-channel`) do not apply to earpiece/speaker.
   - **`ABOX Call Path Param`** is (re)written during call routing (`setCallPathParamForCall`, "type 1" = apply now). Value, see 1c.
   - Full control lists of these paths: appendix.
2. `voice_set_path(voice, out_device)` → `SecRilSetVoicePath(2, out_device)` → `setVoicePath` → `getDeviceType(device, 0)` → path table → **`SetCallAudioPath(client, path, extra)`** (modem message). Path codes in section 6. `extra` = 0 unless "extra volume" is on (3 in PTT mode 3).
3. Only the first time (call not yet active):
   - `proxy_start_voice_call(proxy)`:
     1. `handleStartVoiceCall`: closes the FM-radio loop nodes if they were open (card 0 dev 106 playback "FM-RMic-LB", dev 127 capture "FM-RMic-LB Tx"). Normally a no-op.
     2. `voice_rx_start`: if proxy flag +0x1e9 is set **and** the playback device is a speaker device (id 1 or 50..56): `ABOX SIFS1 OUT Switch` = 1 (**UNCONFIRMED** whether the flag is set on S24; see 7). Then `enable_loop_pcmnode(dev 104, PCM_OUT, cfg_rx, write=false)`.
     3. `voice_tx_start`: `enable_loop_pcmnode(dev 123, PCM_IN, cfg_tx)`, then `enable_loop_pcmnode(dev 131, PCM_IN, cfg_rx)`.
     4. `enable_loop_pcmnode` = `pcm_open(0, dev, dir | PCM_MONOTONIC(0x8), &cfg)` + `pcm_is_ready` + **`pcm_start`**. **Nothing is ever written or read.** (The write=true variant writes period_count x 1920 zero bytes instead of pcm_start; calls do not use it.) A failure closes it again and the start returns -1.
     5. Configs (tinyalsa `pcm_config`, 10 x u32):

        | node | channels | rate | period_size | period_count | format | start_thr | stop_thr | silence_thr | silence_size | avail_min |
        |---|---|---|---|---|---|---|---|---|---|---|
        | 104 Rx | 2 | 48000 | 480 | 4 | S16_LE (0) | 480 | 0xFFFFFFFF | 0 | 0 | 0 |
        | 123 Tx | **4** (quad-mic, see 5) or 2 | 48000 | 480 | 4 | S16_LE | 480 | 0xFFFFFFFF | 0 | 0 | 0 |
        | 131 Ref | 2 | 48000 | 480 | 4 | S16_LE | 480 | 0xFFFFFFFF | 0 | 0 | 0 |

        No PCM_MMAP, no PCM_NOIRQ. stop_threshold 0xFFFFFFFF = never stop on xrun, so an open+start with no data keeps running.
   - `voice_set_call_active(voice, true)` → `SecRilSetRealCallStatus(1)`: **only sets a flag inside libaudio-ril (+0x30), no modem message.** HAL state 1 → 2.
   - Special case: if voice +0x25 is 0 and voice +0x20 == 0x100, `voice_set_mic_mute(adev, true)` (a forced-mute state; normally not taken).
   - `diamondvoice_send_dha_data(adev, NULL, 1)` (hearing-aid EQ data; not needed).
   - If ODM +0xa8 == 1: `ABOX OEM EFFECT VIDEO` = 0.
   - log `*** Started CP Voice Call ***`.
4. `voice_set_volume(adev, adev->voice_volume)`: only while the call is active: index = (int)((steps-1) x volume + small rounding) → **`ABOX Speech Volume` = index** (0..7 here). No modem message (libaudio-ril's `setVoiceVolume`/`SetCallVolume` is never called by this HAL).
5. `voice_set_audio_mode(adev, 0, voice+0x2f)` → `diamondvoice_set_audiomode` → **`ABOX Speech NBQuality` = voice+0x2f** (framework param `l_call_nb_quality_enable`, normally 0).
6. `voice_set_mic_mute(adev, adev->mic_mute)` → `ABOX DVTx Tx Mute` = m, and `SecRilSetTxMute(m)` → **`SetMute(client, m ? 1 : 0)`** (modem message).
7. Rx mute handling (only if forced): `ABOX Speech Rx Mute`.

### 1c. `ABOX Call Path Param` (integer control)
Packed: **bits 0-5 = device id, bits 6-7 = band, bits 8-12 = call type** (written as `& 0x7fff`).
- band: `voice->band` from the framework param `g_call_band` = `nb`→0, `wb`→1, `swb`→2, `fb`→3 (anything else → 0). For a CS call a band of 2 is written as 0. Default (never set) = 0 = NB.
- call type for ausage 20: `l_call_rat_type` 1 → 1, 2 → 2, anything else → 0 (default 0). Meaning of the RAT numbers: **UNCONFIRMED** (framework value).
- device id (`get_callparam_device_id`): earpiece → **0** (21 if "NC booster" is supported, 35/36 with mic-mode support, 1/37/38 with a voice +0x1d flag); speaker → **4** (45 with an ODM flag, 22/23/24/46/47 in mic-mode variants). Whether NC booster / mic-mode support is on for S24 is **UNCONFIRMED** (property/config driven).
- Plain CS NB call: earpiece **0x0000**, speaker **0x0004**; WB adds 0x40.
- Cleared to 0 when the mode returns to NORMAL.

---

## 2. CALL END (mode back to NORMAL), in order

1. `adev_set_mode(NORMAL=0)`:
   - `proxy_set_audiomode(proxy, 0)`: `checkClearCallPathParam` → `clearCallPathParam` (cached param = 0); **`ABOX CP call TX MUTE` = 0**, **`ABOX Translator Status` = 0**; **`ABOX Audio Mode` = NORMAL (0)**; `setCallPathParam` → **`ABOX Call Path Param` = 0**.
   - post-condition: `SecRilSetIpcCmd(1, 0)` → modem IPC audio mode 0 (section 6).
2. `AudioHalOdmAdapter::clearVoiceCallStatus(adev)` (reached from the NORMAL case of the mode post-condition, through the ODM adapter vtable +0x18; the exact hook is **UNCONFIRMED**, the body is not):
   1. `voice_set_call_active(voice, false)`: state 2 → 1 (no modem message on false).
   2. `proxy_stop_voice_call(proxy)`:
      - **`ABOX Out Fade start` = 1**
      - `voice_rx_stop`: **`ABOX RDMA11 Func` = `Mute`**; if flag +0x1e9 and speaker device: `ABOX RDMA7 Func` = `Mute`, `ABOX SIFS1 OUT Switch` = 0; then **`pcm_stop` + `pcm_close` on dev 104**; (speaker case: `ABOX RDMA7 Func` = `Normal`); **`ABOX RDMA11 Func` = `Normal`**.
      - `voice_tx_stop`: `pcm_stop` + `pcm_close` on dev 123, then dev 131.
   3. `update_call_stream(out, dev, dev)`: re-routes the primary output for normal mode. The incall paths are reset (`audio_route_reset_path` = controls back to their XML default) and the normal playback path is applied.
   4. `voice_set_call_mode(voice, false)` (state → 0).
   5. `diamondvoice_set_mute(adev, TX, 0)` → **`ABOX DVTx Tx Mute` = 0**.
   6. `diamondvoice_send_dha_data(adev, NULL, 0)`.
   7. `proxy_call_status(proxy, 0)` (sound-trigger notification only).

No `SetCallAudioPath`, `SetMute` or loopback message is sent at call end. The modem only gets the audio-mode IPC (mode 0).

---

## 3. During a call

- **Device change (earpiece ↔ speaker)**: `doStreamRoutingForPlayback` runs again: reset the old paths, apply `incall_nb-speaker` + `gain-incall_nb-speaker` and `incall_nb-speaker-mic` + `gain-incall_nb-speaker-mic`, rewrite Call Path Param (device 4), `SetCallAudioPath(6, extra)`, then volume / NBQuality / mic-mute again. The loop PCMs are not restarted on this path (call already active). The SIFS1 speaker switch is only touched in `voice_rx_start`/`voice_rx_stop`; whether `setRouteForUpdateCallStream` (which calls `proxy_stop_voice_call` and `voice_set_call_active`) restarts the nodes on a device change is **UNCONFIRMED**.
- **Mic mute**: `voice_set_mic_mute(adev, m)`: `ABOX DVTx Tx Mute` = m and modem `SetMute(m ? 1 : 0)` (TX mute 1 / TX unmute 0). Skipped (unmute refused) in the forced-mute states ("force mute", "mic mute").
- **Rx mute**: `voice_set_rx_mute(adev, m)`: `ABOX Speech Rx Mute` = m only (libaudio-ril has `setRxMute` → `SetMute(3/2)`, but this HAL never calls it).
- **Volume**: `ABOX Speech Volume` = 0..steps-1 (steps 8 here). No modem message.
- `ABOX CP call TX MUTE` is only written (to 0) in `proxy_set_audiomode` when leaving the call.

---

## 4. Modem loopback test (no network call needed)

Factory path (`factory_set_parameters`, keys `factory_test_type`, `factory_test_path`/`factory_test_route`, `factory_test_loopback=on|off`):

- `factory_test_type` → loopback mode: `pcm`=1, `packet`=2, `packet_nodelay`=4, `codec`=5, `realtime`=6, `nb`=16, `wb`=17, `swb`=18.
- Device pairs used by the factory paths (out, in): (earpiece 0x1, 0x80040080), (speaker 0x2, BUILTIN_MIC 0x80000004), (speaker, BACK_MIC 0x80000080), (speaker, 0x80040004), (speaker, 0x80000084), (USB headset 0x4000000, 0x82000000), (0x3, 0x80000004 → "RCV (Speaker2)" = treated as earpiece).
- ON:
  1. Call Path Param device field from `get_callparam_factorymode` (cached).
  2. `voice_set_loopback_mode(adev, mode, out_dev, in_dev)`: **`ABOX Audio Mode` = `IN_LOOPBACK` (7)** (6 for mode 6), then `SecRilSetLoopback(mode, out_dev, in_dev)` → **`SetLoopbackTest(client, mode, path)`** with path = same path table as calls (earpiece 1, speaker 6).
  3. `proxy_stop_voice_call` (clean slate), then route the loopback usage: `loopback_packet-handset` + `gain-loopback_packet-handset`, capture `loopback_packet-handset-mic` + `gain-loopback_packet-handset-mic` for packet mode (name construction as in 1b; the exact capture device name for the factory mic pair is **UNCONFIRMED**: `loopback_packet-mic` also exists).
  4. `proxy_start_voice_call` → the **same** loop nodes 104/123/131.
  5. `voice_set_volume`, `voice_set_mic_mute`.
- OFF (`factory_reset_loopback`): `voice_set_loopback_mode(adev, 0, 1, 4)` → `ABOX Audio Mode` back to the current HAL mode (NORMAL) and `SetLoopbackTest(0, 1)`; `proxy_stop_voice_call`; `voice_set_call_mode(false)`; routes reset.
- The packet-loopback paths use the same CP links as a call (`ABOX SPUS ASRC11 IS = CP`, `ABOX SPUM ASRC6 OS = CP`, `ASRC0 OS = CP`, VDSP0 `cpcall-rxse-callrec`, VDSP5 `cpcall-seriallif-multimic`, VDSP6 `cpcall-txse-callrec`). So mode 2 ("packet") exercises the AP ↔ ABOX ↔ CP voice path end to end without dialing: speak into the mic, hear yourself in the earpiece.

---

## 5. Mic count, channel map, echo reference

- `getPcmconfig(aproxy, 123)` returns the **quad-mic config (4 ch, 48 kHz)** when either the active capture device id is 155..158, or the usage is a CP call (`is_usage_CPCall`) and the capture device id is one of {100, 108, 110, 111, 150, 151, 152}, and in both cases a CP-call usage is active. Otherwise 2 ch. Which id `handset-mic` / `speaker-mic` has is **UNCONFIRMED** (the proxy's table is built at runtime from an initializer list; only ids 0-3 and some >= 156 were decoded). The call mic paths themselves are 4-channel throughout (`ABOX UAIF6 Channel 4`, `WDMA6 4ch 32 kHz`, `WDMA1 4ch 48 kHz`, `DMIC1..3` + VTS DMIC EN0..2). So **open node 123 with 4 channels**, and fall back to 2 if hw_params refuses.
- Channel order inside the 4 channels: decided in the DSP (`VDSP5 = cpcall-seriallif-multimic`, `VDSP4 = wdma1-tdmdemux0-normal-capture`). The Linux side never reads the samples, so it doesn't matter for a call.
- Echo reference: node 131, 2 ch / 48 kHz; the path routes UAIF3 (amp feedback) → NSRC0 → WDMA0 (4 ch 32 kHz) with `ABOX SPUM ASRC0 OS = CP`.

---

## 6. Modem commands (libsecril-client), exact bytes

Socket: abstract unix stream socket **`VND_Multiclient`** (SIM slot 1; `VND_Multiclient2` = second
RIL). Every request (`SendOemRequestHookRaw`): a 4-byte **big-endian** length of the parcel, then the
parcel: `int32 59` (RIL_REQUEST_OEM_HOOK_RAW), `int32 token`, `int32 len`, `len` bytes padded to a
multiple of 4 (Parcel::Write). Little-endian ints inside the parcel. Responses come back on the same
socket; their layout was not decoded here (**UNCONFIRMED**, presumably classic RIL `[BE length][int32
type][int32 token][int32 error][data]`). A client must at least read and drain them. On a write error
the client disconnects.

| function (HAL use) | OEM raw bytes | notes |
|---|---|---|
| `SendOemIpcCommand(9, 0x0f, 3, mode)` (every audio mode change) | `08 12 00 0c 08 00 00 00 09 0f 03 MM` | a Samsung IPC frame inside: total length BE 12, IPC length LE 8, mseq 0, aseq 0, main 0x09, sub 0x0f, type 3 (SET), data = mode (0 NORMAL, 1 RINGTONE, 2 IN_CALL, 3 IN_COMMUNICATION) |
| `SetCallAudioPath(path, extra)` (call routing) | `08 05 00 06 PP EE` | request-history id 0x66 |
| `SetMute(cond)` (mic mute) | `08 0b 00 05 CC` | 0 TX unmute, 1 TX mute, 2 RX unmute, 3 RX mute |
| `SetLoopbackTest(mode, path)` (factory) | `08 08 00 06 MM PP` | id 0x6e; modes as in section 4; 0 = off |
| `SetCallClockSync`, `SetAudioMode`, `SetSoundClockMode`, `SetCallVolume` | not called by this HAL/libaudio-ril on this phone | the old "clock sync / audio mode" plan is not what stock does here |

Path codes (`getDeviceType` + table at libaudio-ril 0x29d0), plain CS call (no PTT/VoLTE/HAC/DeX):
earpiece **1**, speaker **6** (DeX speaker 15, dual-speaker 18/19), wired headset 2, headphone 7,
BT SCO 4 (NR on 8 kHz: 8, WB 16 kHz: 11, NR+WB: 12), BLE 5, USB headset 17 (16 with USB mic state),
HAC earpiece 13, call forwarding 50. The second byte (`extra`) is 0 normally.

---

## 7. Reproduction notes for Linux

- Mixer integer controls on this kernel report `min=0,max=0` (e.g. `ABOX Call Path Param`, `ABOX Speech Volume`, `ABOX DVTx Tx Mute`). **amixer clamps parsed values to [min,max]**, so set those with a small alsa-lib (`snd_ctl_elem_value_set_integer`) or tinymix-style program, not `amixer cset`. Enums (`ABOX Audio Mode`, `ABOX RDMA11 Func`, `ABOX Sound Type`) and booleans are fine with amixer.
- Applying a path = set each control of the flattened list (appendix) in order. Resetting = set them back to the XML default (third column). For controls with no XML default, audio_route uses the value read from the hardware at init, so read them before applying.
- The HAL also sets `ABOX Tickle` (bool) once at route init, before `audio_route_init`. **UNCONFIRMED** whether it matters.
- PCM nodes with alsa-lib: `hw:0,104` playback / `hw:0,123` and `hw:0,131` capture, RW_INTERLEAVED, S16_LE, 48000, period 480, 4 periods, start_threshold 480, stop_threshold = boundary, then `snd_pcm_start()` without writing or reading; keep them open for the whole call; `snd_pcm_drop` + close at the end.
- Order that matches stock: mode (Audio Mode + IPC) → paths (+ gains) + Call Path Param → `SetCallAudioPath` → open/start 104, 123, 131 → volume / NBQuality / `SetMute(0)` + `ABOX DVTx Tx Mute`=0. End: Audio Mode NORMAL + CP call TX MUTE 0 + Translator 0 + Call Path Param 0 + IPC mode 0 → Out Fade start 1, RDMA11 Func Mute, close 104, RDMA11 Func Normal, close 123 and 131 → reset paths → DVTx Tx Mute 0.
- UNCONFIRMED flag +0x1e9 (SIFS1 speaker switch): its writer was not found in libaudioproxy2 (no byte store; maybe a wider store or set from config). On a speaker call, if there's no sound, try `ABOX SIFS1 OUT Switch` = 1 (it reads `on` already on the running Linux system).

---

## 8. Open points (UNCONFIRMED, why)

1. Contents of `dev_map_samplingrate_voice` (band → usage): no writer found in the HAL's code; empty would mean always ausage 20. It doesn't matter for the route (NB and WB paths are identical).
2. Capture device id of `handset-mic` / `speaker-mic` (decides the 4 vs 2 channel Tx config): runtime-built table, only partly decoded.
3. NC-booster / mic-mode Call Path Param variants for this model (property or config driven).
4. `l_call_rat_type` meaning (call type bits); default 0.
5. Where the band comes from on stock: framework parameter `g_call_band`. The secril unsolicited callback registered by the HAL (id 11079, `onUnsolSoundNoti`) only logs.
6. secril response framing (not needed to send; needed to parse replies).
7. The exact hook from mode NORMAL to `clearVoiceCallStatus` (vtable slot +0x18 of the ODM adapter, not resolved).

---

## Appendix: flattened call and loopback paths (from mixer_paths.xml + mixer_gains.xml)

Third column = value audio_route resets to (top-level `<ctl>` defaults in both files). `[n]` = control element id.
NB and WB variants are identical, so only `incall_nb-*` is listed.

#### `incall_nb-handset` (41 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left ASPTX1 Slot Position | 0 | (not in defaults: read before applying) |
| Left ASPTX2 Slot Position | 2 | (not in defaults: read before applying) |
| Left ASPTX3 Slot Position | 4 | (not in defaults: read before applying) |
| Left ASPTX4 Slot Position | 6 | (not in defaults: read before applying) |
| Left ASP TX1 Source | ASPRX1 | (not in defaults: read before applying) |
| Left ASP TX2 Source | VMON | (not in defaults: read before applying) |
| Left ASP TX3 Source | Zero | (not in defaults: read before applying) |
| Left ASP TX4 Source | Zero | (not in defaults: read before applying) |
| Left PCM Source | ASPRX1 | DSP |
| Left Boost Enable | Enabled | Disabled |
| Left AMP Enable Switch | 1 | 0 |
| RECEIVER Switch | 1 | 0 |
| ABOX SIFS0 Width | 32 | 24 |
| ABOX SIFS0 Channel | 4 | 2 |
| ABOX UAIF3 SPK | SIFS0 | RESERVED |
| ABOX SPUS OUT0 | SIFS0 | RESERVED |
| ABOX SPUS OUT1 | SIFS0 | RESERVED |
| ABOX SPUS OUT2 | SIFS0 | RESERVED |
| ABOX SPUS OUT3 | SIFS0 | RESERVED |
| ABOX SPUS OUT5 | SIFS0 | RESERVED |
| ABOX SPUS OUT6 | SIFS0 | RESERVED |
| ABOX SPUS OUT9 | SIFS0 | RESERVED |
| ABOX SPUS OUT11 | SIFS0 | RESERVED |
| ABOX SPUS OUT13 | SIFS0 | RESERVED |
| ABOX SPUS OUT14 | SIFS0 | RESERVED |
| ABOX SPUS OUT15 | SIFS0 | RESERVED |
| ABOX NSRC8 Bridge | 1 | 0 |
| ABOX NSRC8 | SIFS0 | RESERVED |
| ABOX SIFM8 CH0 Switch | 1 | (not in defaults: read before applying) |
| ABOX SIFM8 CH1 Switch | 1 | (not in defaults: read before applying) |
| ABOX RDMA11 Width | 16 | 16 |
| ABOX RDMA11 Channel | 2 | 2 |
| ABOX RDMA11 Rate | 32000 | 48000 |
| ABOX RDMA11 Period | 640 | 480 |
| ABOX RDMA11 Sbank Size | 128 | 0 |
| ABOX SPUS ASRC11 IS | CP | ABOX |
| ABOX SPUS ASRC11 Dummy Start | 1 | 0 |
| ABOX RDMA11 Dummy Start | 1 | 0 |
| ABOX MIXP Dummy Start | 1 | 0 |
| ABOX VDSP0 | cpcall-rxse-callrec | none |
| ABOX Sound Type | VOICE | (not in defaults: read before applying) |

#### `gain-incall_nb-handset` (2 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left Amp Gain | 0 | 15 |
| Left Digital PCM Volume | 817 | 817 |

#### `incall_nb-handset-mic` (43 controls)

| control | set to | reset (XML default) |
|---|---|---|
| DMIC1 Switch | 1 | 0 |
| DMIC2 Switch | 1 | 0 |
| DMIC3 Switch | 1 | 0 |
| VTS DMIC EN0 | enable | disable |
| VTS DMIC EN1 | enable | disable |
| VTS DMIC EN2 | enable | disable |
| ABOX WDMA6 Width | 16 | 16 |
| ABOX WDMA6 Channel | 4 | 2 |
| ABOX WDMA6 Rate | 32000 | 48000 |
| ABOX WDMA6 Period | 64 | 480 |
| ABOX UAIF6 Width | 16 | 16 |
| ABOX UAIF6 Channel | 4 | 4 |
| ABOX UAIF6 Rate | 48000 | 48000 |
| ABOX WDMA1 Width | 16 | 16 |
| ABOX WDMA1 Channel | 4 | 2 |
| ABOX WDMA1 Rate | 48000 | 48000 |
| ABOX WDMA1 Period | 96 | 240 |
| ABOX NSRC6 | UAIF6 | RESERVED |
| ABOX SIFM6 | WDMA | WDMA |
| ABOX NSRC1 Bridge | 1 | 0 |
| ABOX NSRC1 | UAIF6 | RESERVED |
| ABOX SIFM1 | WDMA | WDMA |
| ABOX SPUM ASRC6 OS | CP | (not in defaults: read before applying) |
| ABOX WDMA0 Width | 16 | 16 |
| ABOX WDMA0 Channel | 4 | 2 |
| ABOX WDMA0 Rate | 32000 | 48000 |
| ABOX WDMA0 Period | 64 | 480 |
| ABOX NSRC0 | UAIF3 | RESERVED |
| ABOX SIFM0 | WDMA | WDMA |
| ABOX SPUM ASRC0 OS | CP | ABOX |
| ABOX WDMA7 Width | 16 | 16 |
| ABOX WDMA7 Channel | 2 | 2 |
| ABOX WDMA7 Rate | 32000 | 48000 |
| ABOX WDMA7 Period | 640 | 480 |
| ABOX NSRC7 | SIFS5 | RESERVED |
| ABOX SIFM7 | WDMA | WDMA |
| ABOX SPUS OUT8 | SIFS5 | RESERVED |
| ABOX SIFS5 | SPUS OUT8 | (not in defaults: read before applying) |
| ABOX FWDOUT_2 | WDMA7 | (not in defaults: read before applying) |
| ABOX VDSP4 | wdma1-tdmdemux0-normal-capture | none |
| ABOX VDSP6 | cpcall-txse-callrec | none |
| ABOX OEM NXP Mode | 1 | (not in defaults: read before applying) |
| ABOX VDSP5 | cpcall-seriallif-multimic | none |

#### `gain-incall_nb-handset-mic` (6 controls)

| control | set to | reset (XML default) |
|---|---|---|
| VTS DMIC GAIN MODE0 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE1 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE2 | 6168 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN0 | 18 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN1 | 18 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN2 | 18 | (not in defaults: read before applying) |

#### `incall_nb-speaker` (54 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left ASPTX1 Slot Position | 0 | (not in defaults: read before applying) |
| Left ASPTX2 Slot Position | 2 | (not in defaults: read before applying) |
| Left ASPTX3 Slot Position | 4 | (not in defaults: read before applying) |
| Left ASPTX4 Slot Position | 6 | (not in defaults: read before applying) |
| Left ASP TX1 Source | DSP | (not in defaults: read before applying) |
| Left ASP TX2 Source | ASPRX1 | (not in defaults: read before applying) |
| Left ASP TX3 Source | Zero | (not in defaults: read before applying) |
| Left ASP TX4 Source | Zero | (not in defaults: read before applying) |
| Left PCM Source | DSP | DSP |
| Left Boost Enable | Enabled | Disabled |
| Left AMP Enable Switch | 1 | 0 |
| Right ASPTX1 Slot Position | 1 | (not in defaults: read before applying) |
| Right ASPTX2 Slot Position | 3 | (not in defaults: read before applying) |
| Right ASPTX3 Slot Position | 5 | (not in defaults: read before applying) |
| Right ASPTX4 Slot Position | 7 | (not in defaults: read before applying) |
| Right ASP TX1 Source | DSP | (not in defaults: read before applying) |
| Right ASP TX2 Source | ASPRX1 | (not in defaults: read before applying) |
| Right ASP TX3 Source | Zero | (not in defaults: read before applying) |
| Right ASP TX4 Source | Zero | (not in defaults: read before applying) |
| Right PCM Source | DSP | DSP |
| Right Boost Enable | Enabled | Disabled |
| Right AMP Enable Switch | 1 | 0 |
| RECEIVER Switch | 1 | 0 |
| SPEAKER Switch | 1 | 0 |
| ABOX SIFS0 Width | 32 | 24 |
| ABOX SIFS0 Channel | 4 | 2 |
| ABOX UAIF3 SPK | SIFS0 | RESERVED |
| ABOX SPUS OUT0 | SIFS0 | RESERVED |
| ABOX SPUS OUT1 | SIFS0 | RESERVED |
| ABOX SPUS OUT2 | SIFS0 | RESERVED |
| ABOX SPUS OUT3 | SIFS0 | RESERVED |
| ABOX SPUS OUT5 | SIFS0 | RESERVED |
| ABOX SPUS OUT6 | SIFS0 | RESERVED |
| ABOX SPUS OUT9 | SIFS0 | RESERVED |
| ABOX SPUS OUT11 | SIFS0 | RESERVED |
| ABOX SPUS OUT13 | SIFS0 | RESERVED |
| ABOX SPUS OUT14 | SIFS0 | RESERVED |
| ABOX SPUS OUT15 | SIFS0 | RESERVED |
| ABOX NSRC8 Bridge | 1 | 0 |
| ABOX NSRC8 | SIFS0 | RESERVED |
| ABOX SIFM8 CH0 Switch | 1 | (not in defaults: read before applying) |
| ABOX SIFM8 CH1 Switch | 1 | (not in defaults: read before applying) |
| ABOX RDMA11 Width | 16 | 16 |
| ABOX RDMA11 Channel | 2 | 2 |
| ABOX RDMA11 Rate | 32000 | 48000 |
| ABOX RDMA11 Period | 640 | 480 |
| ABOX RDMA11 Sbank Size | 128 | 0 |
| ABOX SPUS ASRC11 IS | CP | ABOX |
| ABOX SPUS ASRC11 Dummy Start | 1 | 0 |
| ABOX RDMA11 Dummy Start | 1 | 0 |
| ABOX MIXP Dummy Start | 1 | 0 |
| ABOX VDSP0 | cpcall-rxse-callrec | none |
| ABOX VDSP3 | rdmas-sb-vpcmout | rdmas-vpcmout |
| ABOX Sound Type | SPEAKER | (not in defaults: read before applying) |

#### `gain-incall_nb-speaker` (4 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left Amp Gain | 18 | 15 |
| Left Digital PCM Volume | 817 | 817 |
| Right Amp Gain | 18 | 17 |
| Right Digital PCM Volume | 817 | 817 |

#### `incall_nb-speaker-mic` (43 controls)

| control | set to | reset (XML default) |
|---|---|---|
| DMIC1 Switch | 1 | 0 |
| DMIC2 Switch | 1 | 0 |
| DMIC3 Switch | 1 | 0 |
| VTS DMIC EN0 | enable | disable |
| VTS DMIC EN1 | enable | disable |
| VTS DMIC EN2 | enable | disable |
| ABOX WDMA6 Width | 16 | 16 |
| ABOX WDMA6 Channel | 4 | 2 |
| ABOX WDMA6 Rate | 32000 | 48000 |
| ABOX WDMA6 Period | 64 | 480 |
| ABOX UAIF6 Width | 16 | 16 |
| ABOX UAIF6 Channel | 4 | 4 |
| ABOX UAIF6 Rate | 48000 | 48000 |
| ABOX WDMA1 Width | 16 | 16 |
| ABOX WDMA1 Channel | 4 | 2 |
| ABOX WDMA1 Rate | 48000 | 48000 |
| ABOX WDMA1 Period | 96 | 240 |
| ABOX NSRC6 | UAIF6 | RESERVED |
| ABOX SIFM6 | WDMA | WDMA |
| ABOX NSRC1 Bridge | 1 | 0 |
| ABOX NSRC1 | UAIF6 | RESERVED |
| ABOX SIFM1 | WDMA | WDMA |
| ABOX SPUM ASRC6 OS | CP | (not in defaults: read before applying) |
| ABOX WDMA0 Width | 16 | 16 |
| ABOX WDMA0 Channel | 4 | 2 |
| ABOX WDMA0 Rate | 32000 | 48000 |
| ABOX WDMA0 Period | 64 | 480 |
| ABOX NSRC0 | UAIF3 | RESERVED |
| ABOX SIFM0 | WDMA | WDMA |
| ABOX SPUM ASRC0 OS | CP | ABOX |
| ABOX WDMA7 Width | 16 | 16 |
| ABOX WDMA7 Channel | 2 | 2 |
| ABOX WDMA7 Rate | 32000 | 48000 |
| ABOX WDMA7 Period | 640 | 480 |
| ABOX NSRC7 | SIFS5 | RESERVED |
| ABOX SIFM7 | WDMA | WDMA |
| ABOX SPUS OUT8 | SIFS5 | RESERVED |
| ABOX SIFS5 | SPUS OUT8 | (not in defaults: read before applying) |
| ABOX FWDOUT_2 | WDMA7 | (not in defaults: read before applying) |
| ABOX VDSP4 | wdma1-tdmdemux0-normal-capture | none |
| ABOX VDSP6 | cpcall-txse-callrec | none |
| ABOX OEM NXP Mode | 1 | (not in defaults: read before applying) |
| ABOX VDSP5 | cpcall-seriallif-multimic | none |

#### `gain-incall_nb-speaker-mic` (6 controls)

| control | set to | reset (XML default) |
|---|---|---|
| VTS DMIC GAIN MODE0 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE1 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE2 | 6168 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN0 | 8 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN1 | 8 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN2 | 8 | (not in defaults: read before applying) |

#### `loopback_packet-handset` (41 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left ASPTX1 Slot Position | 0 | (not in defaults: read before applying) |
| Left ASPTX2 Slot Position | 2 | (not in defaults: read before applying) |
| Left ASPTX3 Slot Position | 4 | (not in defaults: read before applying) |
| Left ASPTX4 Slot Position | 6 | (not in defaults: read before applying) |
| Left ASP TX1 Source | ASPRX1 | (not in defaults: read before applying) |
| Left ASP TX2 Source | VMON | (not in defaults: read before applying) |
| Left ASP TX3 Source | Zero | (not in defaults: read before applying) |
| Left ASP TX4 Source | Zero | (not in defaults: read before applying) |
| Left PCM Source | ASPRX1 | DSP |
| Left Boost Enable | Enabled | Disabled |
| Left AMP Enable Switch | 1 | 0 |
| RECEIVER Switch | 1 | 0 |
| ABOX SIFS0 Width | 32 | 24 |
| ABOX SIFS0 Channel | 4 | 2 |
| ABOX UAIF3 SPK | SIFS0 | RESERVED |
| ABOX SPUS OUT0 | SIFS0 | RESERVED |
| ABOX SPUS OUT1 | SIFS0 | RESERVED |
| ABOX SPUS OUT2 | SIFS0 | RESERVED |
| ABOX SPUS OUT3 | SIFS0 | RESERVED |
| ABOX SPUS OUT5 | SIFS0 | RESERVED |
| ABOX SPUS OUT6 | SIFS0 | RESERVED |
| ABOX SPUS OUT9 | SIFS0 | RESERVED |
| ABOX SPUS OUT11 | SIFS0 | RESERVED |
| ABOX SPUS OUT13 | SIFS0 | RESERVED |
| ABOX SPUS OUT14 | SIFS0 | RESERVED |
| ABOX SPUS OUT15 | SIFS0 | RESERVED |
| ABOX NSRC8 Bridge | 1 | 0 |
| ABOX NSRC8 | SIFS0 | RESERVED |
| ABOX SIFM8 CH0 Switch | 1 | (not in defaults: read before applying) |
| ABOX SIFM8 CH1 Switch | 1 | (not in defaults: read before applying) |
| ABOX RDMA11 Width | 16 | 16 |
| ABOX RDMA11 Channel | 2 | 2 |
| ABOX RDMA11 Rate | 32000 | 48000 |
| ABOX RDMA11 Period | 640 | 480 |
| ABOX RDMA11 Sbank Size | 128 | 0 |
| ABOX SPUS ASRC11 IS | CP | ABOX |
| ABOX SPUS ASRC11 Dummy Start | 1 | 0 |
| ABOX RDMA11 Dummy Start | 1 | 0 |
| ABOX MIXP Dummy Start | 1 | 0 |
| ABOX VDSP0 | cpcall-rxse-callrec | none |
| ABOX Sound Type | VOICE | (not in defaults: read before applying) |

#### `gain-loopback_packet-handset` (2 controls)

| control | set to | reset (XML default) |
|---|---|---|
| Left Amp Gain | 4 | 15 |
| Left Digital PCM Volume | 761 | 817 |

#### `loopback_packet-handset-mic` (43 controls)

| control | set to | reset (XML default) |
|---|---|---|
| DMIC1 Switch | 1 | 0 |
| DMIC2 Switch | 1 | 0 |
| DMIC3 Switch | 1 | 0 |
| VTS DMIC EN0 | enable | disable |
| VTS DMIC EN1 | enable | disable |
| VTS DMIC EN2 | enable | disable |
| ABOX WDMA6 Width | 16 | 16 |
| ABOX WDMA6 Channel | 4 | 2 |
| ABOX WDMA6 Rate | 32000 | 48000 |
| ABOX WDMA6 Period | 64 | 480 |
| ABOX UAIF6 Width | 16 | 16 |
| ABOX UAIF6 Channel | 4 | 4 |
| ABOX UAIF6 Rate | 48000 | 48000 |
| ABOX WDMA1 Width | 16 | 16 |
| ABOX WDMA1 Channel | 4 | 2 |
| ABOX WDMA1 Rate | 48000 | 48000 |
| ABOX WDMA1 Period | 96 | 240 |
| ABOX NSRC6 | UAIF6 | RESERVED |
| ABOX SIFM6 | WDMA | WDMA |
| ABOX NSRC1 Bridge | 1 | 0 |
| ABOX NSRC1 | UAIF6 | RESERVED |
| ABOX SIFM1 | WDMA | WDMA |
| ABOX SPUM ASRC6 OS | CP | (not in defaults: read before applying) |
| ABOX WDMA0 Width | 16 | 16 |
| ABOX WDMA0 Channel | 4 | 2 |
| ABOX WDMA0 Rate | 32000 | 48000 |
| ABOX WDMA0 Period | 64 | 480 |
| ABOX NSRC0 | UAIF3 | RESERVED |
| ABOX SIFM0 | WDMA | WDMA |
| ABOX SPUM ASRC0 OS | CP | ABOX |
| ABOX WDMA7 Width | 16 | 16 |
| ABOX WDMA7 Channel | 2 | 2 |
| ABOX WDMA7 Rate | 32000 | 48000 |
| ABOX WDMA7 Period | 640 | 480 |
| ABOX NSRC7 | SIFS5 | RESERVED |
| ABOX SIFM7 | WDMA | WDMA |
| ABOX SPUS OUT8 | SIFS5 | RESERVED |
| ABOX SIFS5 | SPUS OUT8 | (not in defaults: read before applying) |
| ABOX FWDOUT_2 | WDMA7 | (not in defaults: read before applying) |
| ABOX VDSP4 | wdma1-tdmdemux0-normal-capture | none |
| ABOX VDSP6 | cpcall-txse-callrec | none |
| ABOX OEM NXP Mode | 1 | (not in defaults: read before applying) |
| ABOX VDSP5 | cpcall-seriallif-multimic | none |

#### `gain-loopback_packet-handset-mic` (6 controls)

| control | set to | reset (XML default) |
|---|---|---|
| VTS DMIC GAIN MODE0 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE1 | 6168 | (not in defaults: read before applying) |
| VTS DMIC GAIN MODE2 | 6168 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN0 | 27 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN1 | 17 | (not in defaults: read before applying) |
| VTS CONTROL DMIC AUD GAIN2 | 27 | (not in defaults: read before applying) |
