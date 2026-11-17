export WLR_RENDERER=gles2
export WLR_DRM_DEVICES=/dev/dri/card1
export WLR_NO_HARDWARE_CURSORS=1
export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
export MESA_LOADER_DRIVER_OVERRIDE=zink
export LD_PRELOAD="/usr/local/lib/libs24fbfix.so /usr/local/lib/libs24kwin.so"  # s24kwin: only acts in kwin_wayland
export GSK_RENDERER=vulkan
# KWin (Plasma) on the S24 (Claude, 2026-11-15): the panel is exynos-drm card1, the GPU renders on renderD128 (Zink)
export KWIN_DRM_DEVICES=/dev/dri/card1
export KWIN_RENDER_NODES=/dev/dri/renderD128
