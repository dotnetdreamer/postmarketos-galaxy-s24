/* s24fbfix: LD_PRELOAD shim for compositors on the Galaxy S24 (Claude, 2026-11-14).
 * Samsung's exynos-drmdpu display driver reads a framebuffer's lines as width * cpp bytes apart and
 * ignores the pitch. GPU (sgpu/radv-xclipse) linear buffers have their pitch padded to 256 bytes
 * (1080 px -> 4352 bytes = 1088 px), so the panel showed every line shifted: horizontal stripes.
 * Fix: register such framebuffers as pitch / cpp pixels wide; the plane's source rectangle still
 * selects the real 1080 columns, so the padding is never shown. Single-plane 32-bit RGB only. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int is_rgb32(uint32_t f)
{
	return f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888 || f == DRM_FORMAT_XBGR8888 ||
	       f == DRM_FORMAT_ABGR8888 || f == DRM_FORMAT_RGBX8888 || f == DRM_FORMAT_RGBA8888 ||
	       f == DRM_FORMAT_BGRX8888 || f == DRM_FORMAT_BGRA8888 || f == DRM_FORMAT_XRGB2101010 ||
	       f == DRM_FORMAT_ARGB2101010 || f == DRM_FORMAT_XBGR2101010 || f == DRM_FORMAT_ABGR2101010;
}

static uint32_t fixed_width(uint32_t width, uint32_t format, const uint32_t pitches[4])
{
	if (is_rgb32(format) && pitches[0] && !pitches[1] && pitches[0] % 4 == 0 && pitches[0] / 4 > width)
		return pitches[0] / 4;
	return width;
}

int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
			       const uint32_t bo_handles[4], const uint32_t pitches[4], const uint32_t offsets[4],
			       const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags)
{
	static int (*real)(int, uint32_t, uint32_t, uint32_t, const uint32_t *, const uint32_t *,
			   const uint32_t *, const uint64_t *, uint32_t *, uint32_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "drmModeAddFB2WithModifiers");
	return real(fd, fixed_width(width, pixel_format, pitches), height, pixel_format, bo_handles, pitches,
		    offsets, modifier, buf_id, flags);
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4],
		  const uint32_t pitches[4], const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags)
{
	static int (*real)(int, uint32_t, uint32_t, uint32_t, const uint32_t *, const uint32_t *,
			   const uint32_t *, uint32_t *, uint32_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "drmModeAddFB2");
	return real(fd, fixed_width(width, pixel_format, pitches), height, pixel_format, bo_handles, pitches,
		    offsets, buf_id, flags);
}
