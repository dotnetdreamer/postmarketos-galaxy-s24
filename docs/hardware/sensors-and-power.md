# Sensors, double tap to wake, deep sleep

## Sensor hub

**State:** orientation (auto-rotate), ambient light and proximity work through `net.hadess.SensorProxy`, so Plasma and Phosh use them unchanged. Auto-rotation and "near" were not checked physically.

Samsung's sensors sit behind a Cortex-M sensor hub ("CHUB", Samsung's `shub` driver). Chips: LSM6DSV (accelerometer, gyroscope), AK09918C (compass), STK33F11 (light, proximity), LPS22DF (barometer).

- Boot the hub: `echo 1 > /sys/class/sensors/ssp_sensor/fs_ready` (loads `/vendor/firmware/os.checked.bin`). It runs when reading `ssp_sensor/sensor_spec` succeeds
- Enable a sensor: `ioctl(/dev/batch_io, 0xFC00 | type, {max_latency_ns, period_ns, flags})`, then write `type*10+1` to `ssp_sensor/enable`
- Read it from the IIO device named after it (`buffer/enable` = 1; records = payload + u64 timestamp)
- Types: accelerometer 1 (s16 xyz, 2048 per g), light 5 (u32 lux), proximity 8 (u8, 1 = near), device orientation 27 (u8 0 to 3, 255 = flat)

`rootfs/usr/local/sbin/s24-sensord` (Python, service `s24-sensors`) provides SensorProxy and turns each sensor on only while an app claims it. `iio-sensor-proxy` can't read this hub: take it out of the runlevel. `tests/sensors/sensortest.py` reads any hub sensor by type.

## Screen off and double tap to wake

**State:** works.

- The touch controller follows the display state through `/sys/class/sec/tsp/enabled` (`1,0` off, `2,1` on), like Android does
- `aot_enable,1` on `/sys/class/sec/tsp/cmd` arms double tap: in low-power mode the chip sends `KEY_WAKEUP`
- Phosh lights the screen on `KEY_WAKEUP`; **KWin ignores it on purpose** while the screen is off, so under Plasma `s24-powerd` answers a double tap with an injected power key press
- Pocket check: after a double tap the proximity sensor is read; if something is near, the screen goes off again

## Deep sleep

**State:** works. `mem_sleep` is `deep`. Wakes on power key, double tap, Wi-Fi traffic (wake on pattern), RTC alarms.

`rootfs/usr/local/sbin/s24-powerd` (Python, service `s24-power`, settings in `/etc/s24-power.conf`, log `/var/log/s24-power.log`):

1. Watches the panel state (lit: every 2 s, dark: every 1 s)
2. With the screen dark and `autosleep` on, waits for: no logind sleep-block inhibitors, no ALSA stream running, no kernel wake locks, little CPU and network activity
3. Suspends with the kernel's wakeup-count handshake: read `/sys/power/wakeup_count` (blocks while a wakeup source is active), write it back, write `mem`

Rules learned:

- Without the handshake, a suspend with USB data up left the USB controller dead
- With the USB data cable in a PC, USB wakeup sources stay held: no sleep (same as Android). A charger is fine
- logind's own suspend has no handshake and NetworkManager drops Wi-Fi after it: the postmarketOS default "suspend when idle" is turned off (`sleep-inactive-battery-type 'nothing'`), only `s24-powerd` suspends
- A few `EBUSY`/`EAGAIN` refusals right after waking or after a camera closes are normal; it retries
- Any program can keep the phone awake by writing a name to `/sys/power/wake_lock` (and remove it via `wake_unlock`); `s24-cam` does this while a camera is open
- `pm_test` levels `processors` and `core` crash this phone

Tests: `tests/sensors/sleeptest.sh` (switches USB data off to mimic a battery-only phone, counts sleeps, wakes by RTC alarm, brings USB back), `suspendtest.sh`, `taptest.py`.
