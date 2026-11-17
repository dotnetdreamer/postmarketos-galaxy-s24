/* fbgrab (Claude, 2026-11-15): save what a KMS plane is scanning out as a PPM, whoever owns the display.
 * Root only (GETFB2 hands out buffer handles to CAP_SYS_ADMIN). ./fbgrab /dev/dri/card1 59 out.ppm
 * gcc -O2 -o fbgrab fbgrab.c -ldrm -I/usr/include/libdrm */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
int main(int argc, char **argv) {
	int fd = open(argv[1], O_RDWR | O_CLOEXEC);
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1); /* SRC_* are atomic-only properties */
	drmModePlane *pl = drmModeGetPlane(fd, atoi(argv[2]));
	if (!pl || !pl->fb_id) { puts("plane has no framebuffer"); return 1; }
	drmModeFB2 *fb = drmModeGetFB2(fd, pl->fb_id);
	if (!fb || !fb->handles[0]) { puts("GETFB2 gave no handle"); return 1; }
	printf("fb %u: %ux%u %.4s pitch %u modifier 0x%llx\n", pl->fb_id, fb->width, fb->height, (char *)&fb->pixel_format, fb->pitches[0], (unsigned long long)fb->modifier);
	size_t size = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
	uint8_t *p = MAP_FAILED;
	struct drm_mode_map_dumb md = {.handle = fb->handles[0]};
	if (!drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, md.offset);
	if (p == MAP_FAILED) {
		int dfd = -1;
		if (!drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &dfd)) p = mmap(NULL, size, PROT_READ, MAP_SHARED, dfd, 0);
	}
	if (p == MAP_FAILED) { puts("could not map the buffer"); return 1; }
	/* visible width: the plane's source rectangle (16.16) when set, else the fb */
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
	uint32_t w = fb->width, h = fb->height;
	for (uint32_t i = 0; props && i < props->count_props; i++) {
		drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[i]);
		if (pr && !strcmp(pr->name, "SRC_W") && props->prop_values[i]) w = props->prop_values[i] >> 16;
		if (pr && !strcmp(pr->name, "SRC_H") && props->prop_values[i]) h = props->prop_values[i] >> 16;
	}
	FILE *o = fopen(argv[3], "wb");
	fprintf(o, "P6\n%u %u\n255\n", w, h);
	uint32_t f = fb->pixel_format;
	int bgr = f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888;
	int ten = f == DRM_FORMAT_XRGB2101010 || f == DRM_FORMAT_ARGB2101010 || f == DRM_FORMAT_XBGR2101010 || f == DRM_FORMAT_ABGR2101010;
	int tenbgr = f == DRM_FORMAT_XBGR2101010 || f == DRM_FORMAT_ABGR2101010;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = p + fb->offsets[0] + (size_t)y * fb->pitches[0];
		for (uint32_t x = 0; x < w; x++) {
			uint8_t rgb[3];
			if (ten) {
				uint32_t v = ((const uint32_t *)row)[x];
				uint32_t lo = (v & 0x3ff) >> 2, mid = ((v >> 10) & 0x3ff) >> 2, hi = ((v >> 20) & 0x3ff) >> 2;
				rgb[0] = tenbgr ? lo : hi, rgb[1] = mid, rgb[2] = tenbgr ? hi : lo;
			} else {
				const uint8_t *px = row + 4 * x; /* little-endian XRGB: B G R X */
				rgb[0] = bgr ? px[0] : px[2], rgb[1] = px[1], rgb[2] = bgr ? px[2] : px[0];
			}
			fwrite(rgb, 1, 3, o);
		}
	}
	fclose(o);
	printf("saved %ux%u to %s\n", w, h, argv[3]);
	return 0;
}
