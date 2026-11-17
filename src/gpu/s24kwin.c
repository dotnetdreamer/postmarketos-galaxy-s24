/*
 * libs24kwin.so — KWin on the Galaxy S24 (Claude, 2026-11-15). LD_PRELOAD into the Plasma session;
 * does nothing in any program but kwin_wayland.
 *
 * The panel's planes (exynos-drm, card1) list only Samsung's own modifiers in IN_FORMATS (compressed
 * layouts and a "protected" one), never LINEAR, although they scan out plain linear buffers fine
 * (that is what every FB from drmModeAddFB2 is). KWin intersects a plane's modifiers with what the
 * GPU (Mesa Zink on radv-xclipse, renderD128) imports. That leaves nothing, so KWin falls back to
 * implicit modifiers, which Zink refuses to import on RADV ("driver can't handle INVALID<->LINEAR",
 * EGL_BAD_ALLOC), the test commit fails and the panel stays off.
 *
 * Here every format-modifier blob KWin reads gets LINEAR added for all its formats. KWin then
 * allocates its scanout buffers on card1 as LINEAR (256-byte pitch: 1080 px -> 4352), imports them
 * into Zink with an explicit LINEAR modifier and registers them with drmModeAddFB2WithModifiers,
 * where libs24fbfix.so widens the framebuffer to the pitch (the panel ignores the pitch).
 * Checked by hand first (linprobe.c): allocate on card1, import into Zink, clear on the GPU, read
 * back through card1, AddFB2WithModifiers LINEAR: all fine.
 *
 * gcc -shared -fPIC -O2 -o /usr/local/lib/libs24kwin.so s24kwin.c -ldl -I/usr/include/libdrm
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int active = -1;

/* Only KWin: every other program in the session passes straight through. */
static int in_kwin(void)
{
	if (active < 0) {
		char comm[64] = "";
		FILE *f = fopen("/proc/self/comm", "r");
		if (f) {
			if (fgets(comm, sizeof comm, f))
				comm[strcspn(comm, "\n")] = 0;
			fclose(f);
		}
		active = strcmp(comm, "kwin_wayland") == 0;
	}
	return active;
}

/* A struct drm_format_modifier_blob exactly as the kernel lays it out, or not. */
static int is_format_blob(const drmModePropertyBlobRes *b)
{
	const struct drm_format_modifier_blob *h = b->data;
	if (!h || b->length < sizeof *h || h->version != FORMAT_BLOB_CURRENT || h->formats_offset != sizeof *h)
		return 0;
	if (h->count_formats == 0 || h->count_formats > 4096 || h->count_modifiers > 4096)
		return 0;
	uint64_t mods = ((uint64_t)h->formats_offset + 4ull * h->count_formats + 7) & ~7ull;
	return h->modifiers_offset == mods &&
	       b->length >= mods + sizeof(struct drm_format_modifier) * (uint64_t)h->count_modifiers;
}

drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
	static drmModePropertyBlobPtr (*real)(int, uint32_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "drmModeGetPropertyBlob");
	drmModePropertyBlobPtr b = real(fd, blob_id);
	if (!b || !in_kwin() || !is_format_blob(b))
		return b;

	const struct drm_format_modifier_blob *h = b->data;
	const struct drm_format_modifier *m = (const void *)((const char *)b->data + h->modifiers_offset);
	for (uint32_t i = 0; i < h->count_modifiers; i++)
		if (m[i].modifier == DRM_FORMAT_MOD_LINEAR)
			return b; /* already there */

	/* Old header, formats and modifiers, then LINEAR for every format, 64 formats per entry
	 * (last, so libdrm's iterator never reports a spurious INVALID for a format). */
	uint32_t extra = (h->count_formats + 63) / 64;
	size_t old_mods = sizeof *m * h->count_modifiers;
	size_t len = h->modifiers_offset + old_mods + sizeof *m * extra;
	char *data = calloc(1, len);
	if (!data)
		return b;
	memcpy(data, b->data, h->modifiers_offset + old_mods);
	struct drm_format_modifier_blob *nh = (void *)data;
	struct drm_format_modifier *nm = (void *)(data + nh->modifiers_offset + old_mods);
	for (uint32_t j = 0; j < extra; j++) {
		uint32_t n = h->count_formats - 64 * j;
		nm[j].offset = 64 * j;
		nm[j].formats = n >= 64 ? ~0ull : (1ull << n) - 1;
		nm[j].modifier = DRM_FORMAT_MOD_LINEAR;
	}
	nh->count_modifiers += extra;
	/* libdrm allocates both with calloc and drmModeFreePropertyBlob frees both with free */
	free(b->data);
	b->data = data;
	b->length = len;
	return b;
}
