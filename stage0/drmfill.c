/* drmfill: draw a full-screen picture with a text line on a DRM/KMS display using only raw ioctls
 * (no libdrm), as the first visible sign of Linux on the Galaxy S24 (Claude, 2026-11-14).
 *   drmfill [card] [TEXT] [seconds]      e.g. drmfill /dev/dri/card0 "LINUX IS RUNNING" 600
 * Prints what it finds (connectors, modes, CRTCs) and keeps the picture up for [seconds]. */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const unsigned char font[][5] = { /* 5x7, columns, bit 0 = top; ' ' .. 'Z' */
	{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
	{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
	{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
	{0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
	{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
	{0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
	{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
	{0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
	{0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},
};

static int io(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static void text(uint32_t *px, uint32_t stride, uint32_t w, uint32_t h, int x0, int y0, int scale, const char *s, uint32_t color)
{
	for (; *s; s++, x0 += 6 * scale) {
		int c = *s;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c < ' ' || c > 'Z')
			c = '?';
		const unsigned char *g = font[c - ' '];
		for (int col = 0; col < 5; col++)
			for (int row = 0; row < 8; row++)
				if (g[col] & (1 << row))
					for (int dy = 0; dy < scale; dy++)
						for (int dx = 0; dx < scale; dx++) {
							int x = x0 + col * scale + dx, y = y0 + row * scale + dy;
							if (x >= 0 && y >= 0 && (uint32_t)x < w && (uint32_t)y < h)
								px[y * (stride / 4) + x] = color;
						}
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	const char *msg = argc > 2 ? argv[2] : "LINUX IS RUNNING";
	int secs = argc > 3 ? atoi(argv[3]) : 600;
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror(dev); return 1; }
	if (io(fd, DRM_IOCTL_SET_MASTER, 0))
		perror("SET_MASTER (continuing)");
	struct drm_version ver = {0};
	char name[64] = {0};
	ver.name = name; ver.name_len = sizeof name - 1;
	io(fd, DRM_IOCTL_VERSION, &ver);
	printf("driver %s %d.%d.%d\n", name, ver.version_major, ver.version_minor, ver.version_patchlevel);

	struct drm_mode_card_res res = {0};
	if (io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return 1; }
	uint32_t *crtcs = calloc(res.count_crtcs + 1, 4), *conns = calloc(res.count_connectors + 1, 4);
	uint32_t *encs = calloc(res.count_encoders + 1, 4), *fbs = calloc(res.count_fbs + 1, 4);
	res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
	if (io(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES 2"); return 1; }
	printf("%u crtcs, %u connectors, %u encoders, %u fbs, %ux%u max\n", res.count_crtcs, res.count_connectors,
	       res.count_encoders, res.count_fbs, res.max_width, res.max_height);

	struct drm_mode_modeinfo mode = {0};
	uint32_t conn_id = 0, crtc_id = 0;
	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = {.connector_id = conns[i]};
		if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
		struct drm_mode_modeinfo *modes = calloc(c.count_modes + 1, sizeof *modes);
		uint32_t *cencs = calloc(c.count_encoders + 1, 4);
		uint32_t nm = c.count_modes, ne = c.count_encoders;
		memset(&c, 0, sizeof c);
		c.connector_id = conns[i];
		c.count_modes = nm; c.modes_ptr = (uintptr_t)modes;
		c.count_encoders = ne; c.encoders_ptr = (uintptr_t)cencs;
		if (io(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
		printf("connector %u type %u connection %u, %u modes, encoder %u\n", c.connector_id, c.connector_type,
		       c.connection, c.count_modes, c.encoder_id);
		for (uint32_t m = 0; m < c.count_modes && m < 4; m++)
			printf("  mode %s %ux%u@%u\n", modes[m].name, modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh);
		if (conn_id || c.connection != 1 || !c.count_modes)
			continue;
		conn_id = c.connector_id;
		mode = modes[0];
		uint32_t enc_id = c.encoder_id ? c.encoder_id : (c.count_encoders ? cencs[0] : 0);
		struct drm_mode_get_encoder e = {.encoder_id = enc_id};
		if (enc_id && !io(fd, DRM_IOCTL_MODE_GETENCODER, &e)) {
			crtc_id = e.crtc_id;
			for (uint32_t k = 0; !crtc_id && k < res.count_crtcs; k++)
				if (e.possible_crtcs & (1u << k))
					crtc_id = crtcs[k];
		}
		if (!crtc_id && res.count_crtcs)
			crtc_id = crtcs[0];
	}
	if (!conn_id || !crtc_id) { fprintf(stderr, "no connected connector/crtc\n"); return 2; }
	printf("using connector %u, crtc %u, mode %ux%u\n", conn_id, crtc_id, mode.hdisplay, mode.vdisplay);

	struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
	if (io(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("CREATE_DUMB"); return 3; }
	uint32_t fb_id = 0;
	struct drm_mode_fb_cmd2 f2 = {.width = cd.width, .height = cd.height, .pixel_format = DRM_FORMAT_XRGB8888};
	f2.handles[0] = cd.handle; f2.pitches[0] = cd.pitch;
	if (!io(fd, DRM_IOCTL_MODE_ADDFB2, &f2)) {
		fb_id = f2.fb_id;
	} else {
		perror("ADDFB2 XRGB8888, trying ADDFB");
		struct drm_mode_fb_cmd f = {.width = cd.width, .height = cd.height, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle};
		if (io(fd, DRM_IOCTL_MODE_ADDFB, &f)) { perror("ADDFB"); return 4; }
		fb_id = f.fb_id;
	}
	struct drm_mode_map_dumb md = {.handle = cd.handle};
	if (io(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("MAP_DUMB"); return 5; }
	uint32_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) { perror("mmap"); return 6; }
	for (uint32_t y = 0; y < cd.height; y++)
		for (uint32_t x = 0; x < cd.width; x++)
			px[y * (cd.pitch / 4) + x] = (0x10 + y * 0x50 / cd.height) << 16 | (0x30 + x * 0x60 / cd.width) << 8 | 0x60;
	int scale = (int)cd.width / (6 * 18);
	if (scale < 2) scale = 2;
	text(px, cd.pitch, cd.width, cd.height, scale * 3, (int)cd.height / 3, scale, msg, 0xFFFFFF);
	text(px, cd.pitch, cd.width, cd.height, scale * 3, (int)cd.height / 3 + 12 * scale, scale / 2 > 1 ? scale / 2 : 2,
	     "GALAXY S24 - EXYNOS 2400", 0xFFE070);

	struct drm_mode_crtc cr = {.crtc_id = crtc_id, .fb_id = fb_id, .set_connectors_ptr = (uintptr_t)&conn_id,
				   .count_connectors = 1, .mode = mode, .mode_valid = 1};
	if (io(fd, DRM_IOCTL_MODE_SETCRTC, &cr)) { perror("SETCRTC"); return 7; }
	printf("picture up for %d s\n", secs);
	fflush(stdout);
	sleep(secs);
	return 0;
}
