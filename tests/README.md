# Tests and diagnosis scripts

The scripts used while porting, kept because they show how each part was checked. They are written for this phone: run them as root on the phone, after reading them.

- Some run from the **stage-0 shell** (telnet on the cable) and use `/newroot/...` paths; the rest run in postmarketOS
- Several **stop a service**, run their own copy, and start the service again at the end (camera tests do this)
- The newer ones hold a wake lock while they run; for the others keep the screen on or the USB cable in, or the phone may suspend halfway
- Camera tests measure frames on the phone. Two write pictures: `camera/grab.sh` saves JPEG frames to `/tmp`, and the shot-based tests write one raw frame to `/tmp` and delete it at once. Remember the front camera may see your face

| Folder | What |
|---|---|
| `camera/` | Settings comparison, face priority, heat, sleep, screen-off pause, provider recovery, two cameras, latency (`pwlat`, in `src/camera/`) |
| `audio/` | DSP tracing, muted end-to-end check, underruns, a real Chromium playback test |
| `sensors/` | Sensor hub reads, key injection, sleep cycles, double tap |
| `modem/` | Data, DNS64, Wi-Fi-off fallback, NetworkManager toggles, an SMS draft that is never sent |
| `mousemode/` | Mouse mode daemon and session tests against a stand-in touchscreen |
| `gpu/` | Vulkan and GPU app checks in a headless compositor |
| `usb-c/` | USB-C, monitor and dock state, read only (`usbc-state.sh`); docked mode rules against stand-in settings (`dockd-logic-test.py`) |
