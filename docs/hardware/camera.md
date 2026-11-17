# Cameras

**State:** back (50 MP main) and front cameras work in the Camera app (GNOME Snapshot) and in any PipeWire camera app. 1080p at 30 fps, continuous autofocus, about 70 ms from sensor exposure to the app, photos the right way round. A camera only runs (and only keeps the phone awake) while an app shows it; when the screen goes off it pauses.

Not done: the tele and ultra-wide cameras on their own, zoom, Samsung's photo processing (HDR, night mode). The front camera looks a little cool (bluish); the cause isn't found yet (see the end).

## Why it's built this way

Linux camera apps use libcamera or V4L2. Neither can reach this camera: libcamera has no Exynos pipeline, and the kernel exposes only Samsung's own ISP nodes, no standard sensor device. The only thing that drives the camera is Samsung's Android camera HAL. So:

```
Samsung camera HAL (in the Android container: vendor.samsung.hardware.camera.provider-service_64)
   │  AIDL over binder: ICameraProvider / ICameraDevice / ICameraDeviceSession, buffers from Samsung's gralloc
   ▼
s24-cam serve  (src/camera/s24-cam.c, runs as root on postmarketOS)
   │  one PipeWire Video/Source node per camera ("Back Camera", "Front Camera"),
   │  frames rotated upright, timestamped, written straight into PipeWire's buffers
   ▼
GNOME Snapshot, or any app that uses PipeWire cameras
```

## What the HAL needs (each one was a wall)

| Need | Without it | Done by |
|---|---|---|
| Sensor, actuator and flash kernel modules (`is-cis-gn3`, `is-cis-3k1`, `is-cis-3lu`, `is-cis-imx564`, `is-actuator-ak737x`, `is-flash-s2mpb02-i2c`, `smfc`, `scaler`) | No frames | `s24-camera-run` loads them from the stage-0 ramdisk (`/proc/<pid of busybox sh /init>/root/lib/modules`); postmarketOS's `/lib/modules` doesn't have them |
| Samsung's thermal and HyPer services | The HAL half-initialises and crashes somewhere new every run | Started directly in the container's mount namespace (their rc files have no SELinux label) |
| A sensor service with a gyroscope | The HAL waits forever, or aborts | `s24-camd` (`src/camera/s24-camd.c`): a fake `android.frameworks.sensorservice.ISensorManager` with a still accelerometer and gyroscope |
| cameraserver to initialise the provider once | Its "default request" template has 1 entry instead of 95, and opening crashes | `s24-camera-run` starts cameraserver, waits 8 s, stops it; the provider keeps that state |
| SIGPIPE ignored | cameraserver's dropped connections kill the provider | Patched in (`src/camera/patch_hal11.py`) |
| No deliberate crashes on slow timing | Watchdog and destroy paths write to NULL on purpose | Patched |
| The depth-of-field plugin skipped | A null call a few seconds into live preview | Patched |
| The provider on the fast cores with a clock floor | 24 fps instead of 30 | `taskset 0x3f0` + `uclampset -m 512` |
| A wake lock while a camera is open | The phone deep-sleeps mid-stream | `s24-cam` holds `s24-camera` in `/sys/power/wake_lock` |

## Rules for any HAL client

- **Never close a session with frames or results still on the way.** Wait until every buffer is back and the HAL has been quiet for about 150 ms. Otherwise the provider crashes and comes back half-initialised
- **Always close cleanly.** A client killed without `close()` leaves the provider broken for the next one. With a clean close, reopening takes about 40 ms and needs no new priming
- Samsung's gralloc descriptor isn't gralloc4's encoding: it's a 296-byte struct with a fixed 256-byte name (see `allocate()` in `s24-cam.c`). A buffer handle has 2 fds and 91 ints
- Results carry `bufferId` 0: remember which buffer you sent for each frame number
- The HAL keeps request settings you stop sending (settings merge, they don't replace)
- Shutter timestamps are `CLOCK_BOOTTIME`
- AIDL method codes = declaration order in the frozen `aidl_api` files

## PipeWire details that matter

- Each buffer needs a `SPA_META_Header` with a real `pts`; without it GStreamer's sink drops every frame as late and the app freezes after the first few frames
- 8 buffers (pipewiresrc wants at least 8)
- `api.libcamera.location` = `back` / `front`: Snapshot mirrors the preview of anything that isn't `back` (and never mirrors saved photos)
- GStreamer lists the newest node first, so the back camera is created first
- `s24-cam` drives its own graph (`PW_STREAM_FLAG_DRIVER`, one cycle per camera frame) and drops a frame rather than queueing it, which keeps latency low

## Files

| File | Job |
|---|---|
| `src/camera/s24-cam.c` | The client: `list`, `shot ID WxH OUT [N]`, `stream ID WxH` (raw NV12 on stdout), `serve ID[,ID] WxH` (PipeWire). Options are environment variables, listed at the top of the file |
| `src/camera/s24-camd.c` | The fake sensor service |
| `src/camera/patch_hal11.py` | Makes the patched HAL from your own copy (checks the input's md5) |
| `src/camera/pwlat.c` | PipeWire consumer that measures frame latency (with `S24CAM_STAMP=1`) |
| `src/camera/ndkcam.c` | An Android-side NDK test client (needs a bionic build; not used by the port) |
| `rootfs/usr/local/sbin/s24-camera-run` | The whole setup, then `exec s24-cam serve` |
| `rootfs/etc/init.d/s24-camera` | OpenRC service (supervised, restarts the whole setup if the provider dies) |

Camera IDs: `device@1.1/internal/0` = back (a logical camera over wide, tele and ultra-wide), `internal/1` = front.

## Tests

`tests/camera/`: `abtest.sh` (compare request settings on the live service), `facetest.sh`, `thermtest.sh` (10-minute heat test), `sleeptest.sh`, `screentest.sh` (screen-off pause), `recovery-test.sh` (kill the provider, see it come back), `twocam-test.sh`, `crashlog.sh`. Most of them stop the service and run their own copy of the setup; they start the service again at the end.

Measured: 30 fps with 0 drops, 68 to 70 ms from exposure to the frame in `s24-cam`, 0.2 ms more to the app. A closed camera uses no CPU. 10 minutes of streaming: 40 to 42 °C, no throttling, about 12 % battery per hour.

## Open: the bluish front camera

On a plain scene the front camera measures about 5 % more blue than green. Checked and ruled out: the request settings (the HAL's own 95-entry template looks the same) and the factory calibration (the log says it loads). Face-priority exposure and white balance are now on for the front camera (it works, but its effect on skin tones isn't confirmed). The likely cause is that Samsung tunes some processing only for its own camera app: the HAL logs `isSamsungCamera[0]`, and switching that on needs a private Samsung call that hasn't been tried.
