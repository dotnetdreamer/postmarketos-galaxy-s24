/* linprobe (Claude, 2026-11-15): KWin's scanout path on the S24 with an explicit LINEAR modifier.
 * Allocate on card1's GBM (as KWin does), import into Zink on renderD128 with modifier LINEAR,
 * clear it red on the GPU, read it back through card1, and register it as a KMS framebuffer.
 * gcc -O1 -o linprobe linprobe.c /usr/lib/libEGL.so.1 /usr/lib/libgbm.so.1 /usr/lib/libGLESv2.so.2 -ldrm -I/usr/include/libdrm */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
typedef void *EGLDisplay, *EGLContext, *EGLImage;
typedef int32_t EGLint; typedef unsigned EGLenum; typedef unsigned EGLBoolean; typedef intptr_t EGLAttrib;
struct gbm_device; struct gbm_bo;
union gbm_bo_handle { void *ptr; int32_t s32; uint32_t u32; int64_t s64; uint64_t u64; };
EGLDisplay eglGetPlatformDisplay(EGLenum, void *, const EGLAttrib *);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglBindAPI(EGLenum);
EGLContext eglCreateContext(EGLDisplay, void *, EGLContext, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, void *, void *, EGLContext);
void *eglGetProcAddress(const char *);
EGLint eglGetError(void);
const unsigned char *glGetString(unsigned);
void glGenRenderbuffers(int, unsigned *); void glBindRenderbuffer(unsigned, unsigned);
void glGenFramebuffers(int, unsigned *); void glBindFramebuffer(unsigned, unsigned);
void glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned);
unsigned glCheckFramebufferStatus(unsigned);
void glClearColor(float, float, float, float); void glClear(unsigned); void glFinish(void);
struct gbm_device *gbm_create_device(int);
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *, uint32_t, uint32_t, uint32_t, const uint64_t *, unsigned, uint32_t);
uint64_t gbm_bo_get_modifier(struct gbm_bo *);
uint32_t gbm_bo_get_stride(struct gbm_bo *);
int gbm_bo_get_fd(struct gbm_bo *);
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *);
void *gbm_bo_map(struct gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **);
void gbm_bo_unmap(struct gbm_bo *, void *);
void gbm_bo_destroy(struct gbm_bo *);
#define XR24 0x34325258
int main(void) {
	int kms = open("/dev/dri/card1", O_RDWR | O_CLOEXEC), rnd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	struct gbm_device *gk = gbm_create_device(kms), *gr = gbm_create_device(rnd);
	EGLDisplay d = eglGetPlatformDisplay(0x31D7, gr, NULL);
	EGLint ma, mi;
	if (!d || !eglInitialize(d, &ma, &mi)) { printf("eglInitialize FAILED 0x%x\n", eglGetError()); return 1; }
	eglBindAPI(0x30A0);
	EGLint ca[] = {0x3098, 2, 0x3038};
	EGLContext c = eglCreateContext(d, NULL, NULL, ca);
	eglMakeCurrent(d, NULL, NULL, c);
	printf("render: %s\n", glGetString(0x1F01));
	uint64_t lin = 0;
	struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gk, 1080, 2340, XR24, &lin, 1, 1 | 4);
	if (!bo) { puts("card1 allocation FAILED"); return 1; }
	uint32_t pitch = gbm_bo_get_stride(bo);
	printf("card1 bo: modifier 0x%llx pitch %u\n", (unsigned long long)gbm_bo_get_modifier(bo), pitch);
	int fd = gbm_bo_get_fd(bo);
	EGLint ia[] = {0x3057, 1080, 0x3056, 2340, 0x3271, XR24, 0x3272, fd, 0x3273, 0, 0x3274, (EGLint)pitch,
		       0x3443, 0, 0x3444, 0, 0x3038};
	EGLImage (*create)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *) = eglGetProcAddress("eglCreateImageKHR");
	EGLImage img = create(d, NULL, 0x3270, NULL, ia);
	printf("zink import with LINEAR: %s (0x%x)\n", img ? "ok" : "FAILED", eglGetError());
	if (!img) return 1;
	void (*target)(unsigned, void *) = eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	unsigned rb, fb;
	glGenRenderbuffers(1, &rb); glBindRenderbuffer(0x8D41, rb); target(0x8D41, img);
	glGenFramebuffers(1, &fb); glBindFramebuffer(0x8D40, fb); glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb);
	printf("framebuffer status 0x%x (complete=0x8cd5)\n", glCheckFramebufferStatus(0x8D40));
	glClearColor(1, 0, 0, 1); glClear(0x4000); glFinish();
	uint32_t mstride = 0; void *md = NULL;
	uint32_t *px = gbm_bo_map(bo, 0, 0, 1080, 2340, 1, &mstride, &md);
	if (px) {
		printf("readback via card1: first 0x%08x last 0x%08x (red = 0xffff0000), map stride %u\n", px[0], px[(2339 * mstride) / 4 + 1079], mstride);
		gbm_bo_unmap(bo, md);
	} else puts("gbm_bo_map FAILED");
	uint32_t h[4] = {gbm_bo_get_handle(bo).u32}, p[4] = {pitch}, o[4] = {0}, id = 0;
	uint64_t m[4] = {0};
	int r = drmModeAddFB2WithModifiers(kms, pitch / 4, 2340, XR24, h, p, o, m, &id, DRM_MODE_FB_MODIFIERS);
	printf("AddFB2WithModifiers LINEAR (width %u): %d id %u\n", pitch / 4, r, id);
	if (!r) drmModeRmFB(kms, id);
	r = drmModeAddFB2WithModifiers(kms, 1080, 2340, XR24, h, p, o, m, &id, DRM_MODE_FB_MODIFIERS);
	printf("AddFB2WithModifiers LINEAR (width 1080): %d id %u\n", r, id);
	if (!r) drmModeRmFB(kms, id);
	return 0;
}
