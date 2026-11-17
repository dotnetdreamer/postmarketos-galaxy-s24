# Display, touch and GPU

**State:** works. Plasma Mobile (KWin 6.8) and Phosh (phoc/wlroots) both render on the GPU.

## Devices

| Device | Driver | Node |
|---|---|---|
| Panel 1080x2340 (DSI-1), USB-C DP (DP-1, untested) | `exynos-drmdpu` | `/dev/dri/card1` |
| GPU, Xclipse 940 (AMD RDNA based) | Samsung `sgpu` | `/dev/dri/card0`, `/dev/dri/renderD128` |
| Touchscreen | `stm_ts_spi` (`sec_input`) | `sec_touchscreen` input device |

The panel needs its modules loaded in Android's order with softdeps, and the panel id from the kernel command line passed to `mcd_panel` (see [../LESSONS.md](../LESSONS.md), Boot). `drmfill` in the boot ramdisk can paint the screen before anything else runs.

## Vulkan and OpenGL

The GPU gets [radv-xclipse](https://github.com/JimVulkan/radv-xclipse), a Mesa RADV port for Samsung's Xclipse, built natively on the phone:

```sh
sudo sh src/gpu/build-radv-pmos.sh       # about 3 minutes; installs /opt/xclipse + the Vulkan ICD file
vulkaninfo --summary                      # "AMD Radeon Graphics (RADV MAGELLAN CARL)", Vulkan 1.4
```

OpenGL runs on top through Zink (`MESA_LOADER_DRIVER_OVERRIDE=zink`). glmark2: 758 headless at 800x600, 405 full screen on the panel.

## Making the compositors work

- `rootfs/etc/environment` (Phosh/wlroots): `WLR_RENDERER=gles2`, `WLR_DRM_DEVICES=/dev/dri/card1`, `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`, Zink, `GSK_RENDERER=vulkan` for GTK
- `rootfs/etc/profile.d/s24-display.sh` (KWin): `KWIN_DRM_DEVICES=/dev/dri/card1`, `KWIN_RENDER_NODES=/dev/dri/renderD128`, `LD_PRELOAD="libs24fbfix.so libs24kwin.so"`
- `src/gpu/s24fbfix.c`: the display controller ignores the framebuffer pitch; frames are registered with width = pitch / 4
- `src/gpu/s24kwin.c`: adds the LINEAR modifier to the planes' `IN_FORMATS` as KWin reads them (only inside `kwin_wayland`)
- `src/gpu/linprobe.c`: the standalone test that proved linear buffers scan out
- Keep `MESA_LOADER_DRIVER_OVERRIDE` for card1: the kmsro path gives Zink render-only, but RADV rejects the display's dumb buffers

## Tests

- `grim` (as the session user) shows what the GPU drew; `fbgrab /dev/dri/card1 <plane> out.ppm` (as root) shows what the panel scans out. If they differ, it's a scanout problem
- `tests/gpu/`: Vulkan, glmark2 and VS Code GPU checks

## CPU

All 10 cores reach full speed (X4 3207 MHz, A720 2900/2592 MHz, A520 1959 MHz). `rootfs/etc/local.d/s24-perf.start` sets schedutil with a 1 ms rate limit and turns on Samsung's input booster, as Android does. Thermal trips at 90/100/105 °C are handled by the kernel.
