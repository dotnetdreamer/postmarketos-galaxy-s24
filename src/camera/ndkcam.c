// Galaxy S24 (SM-S921B) NDK camera capture, runs INSIDE the Android container (Claude, 2026-11-15).
//
// Uses Android's NDK Camera2 (libcamera2ndk) + AImageReader (libmediandk): the standard Android
// camera client path. cameraserver (started in the container) provides media.camera and does all
// the gralloc buffer allocation through libgui/libui, so this just asks for YUV_420_888 frames and
// copies the planes out. Built as a bionic aarch64 PIE with a hand-written _start (no NDK crt;
// see hello.c). The NDK C ABI is stable, so the prototypes are declared here.
//
//   ndkcam list
//   ndkcam shot ID WxH OUT [FRAMES]    OUT.yuv = I420 (Y then U then V, no padding) + OUT.txt
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- NDK prototypes (stable C ABI) ------------------------------------------------------- */
typedef enum { ACAMERA_OK = 0 } camera_status_t;
typedef enum { AMEDIA_OK = 0, AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE = 0x21264 } media_status_t;
#define FMT_YUV_420_888 0x23
#define TEMPLATE_PREVIEW 1
#define TEMPLATE_STILL_CAPTURE 2

typedef struct ACameraManager ACameraManager;
typedef struct ACameraDevice ACameraDevice;
typedef struct ACameraCaptureSession ACameraCaptureSession;
typedef struct ACaptureRequest ACaptureRequest;
typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;
typedef struct ACaptureSessionOutput ACaptureSessionOutput;
typedef struct ACameraOutputTarget ACameraOutputTarget;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct ANativeWindow ANativeWindow;
typedef struct ACameraMetadata ACameraMetadata;
typedef struct { int numCameras; const char **cameraIds; } ACameraIdList;

typedef void (*dev_cb)(void *, ACameraDevice *);
typedef void (*dev_err_cb)(void *, ACameraDevice *, int);
typedef struct { void *context; dev_cb onDisconnected; dev_err_cb onError; } ACameraDevice_StateCallbacks;
typedef void (*ses_cb)(void *, ACameraCaptureSession *);
typedef struct { void *context; ses_cb onClosed, onReady, onActive; } ACameraCaptureSession_stateCallbacks;
typedef void (*img_cb)(void *, AImageReader *);
typedef struct { void *context; img_cb onImageAvailable; } AImageReader_ImageListener;

extern ACameraManager *ACameraManager_create(void);
extern void ACameraManager_delete(ACameraManager *);
extern camera_status_t ACameraManager_getCameraIdList(ACameraManager *, ACameraIdList **);
extern void ACameraManager_deleteCameraIdList(ACameraIdList *);
extern camera_status_t ACameraManager_getCameraCharacteristics(ACameraManager *, const char *, ACameraMetadata **);
extern camera_status_t ACameraManager_openCamera(ACameraManager *, const char *, ACameraDevice_StateCallbacks *, ACameraDevice **);
extern camera_status_t ACameraDevice_close(ACameraDevice *);
extern camera_status_t ACameraDevice_createCaptureRequest(const ACameraDevice *, int, ACaptureRequest **);
extern camera_status_t ACameraDevice_createCaptureSession(ACameraDevice *, const ACaptureSessionOutputContainer *, const ACameraCaptureSession_stateCallbacks *, ACameraCaptureSession **);
extern camera_status_t ACaptureSessionOutputContainer_create(ACaptureSessionOutputContainer **);
extern void ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer *);
extern camera_status_t ACaptureSessionOutput_create(ANativeWindow *, ACaptureSessionOutput **);
extern void ACaptureSessionOutput_free(ACaptureSessionOutput *);
extern camera_status_t ACaptureSessionOutputContainer_add(ACaptureSessionOutputContainer *, const ACaptureSessionOutput *);
extern camera_status_t ACameraOutputTarget_create(ANativeWindow *, ACameraOutputTarget **);
extern camera_status_t ACaptureRequest_addTarget(ACaptureRequest *, const ACameraOutputTarget *);
extern camera_status_t ACameraCaptureSession_setRepeatingRequest(ACameraCaptureSession *, void *, int, ACaptureRequest **, int *);
extern void ACameraCaptureSession_stopRepeating(ACameraCaptureSession *);
extern void ACameraCaptureSession_close(ACameraCaptureSession *);

extern media_status_t AImageReader_new(int32_t, int32_t, int32_t, int32_t, AImageReader **);
extern void AImageReader_delete(AImageReader *);
extern media_status_t AImageReader_getWindow(AImageReader *, ANativeWindow **);
extern media_status_t AImageReader_setImageListener(AImageReader *, AImageReader_ImageListener *);
extern media_status_t AImageReader_acquireLatestImage(AImageReader *, AImage **);
extern void AImage_delete(AImage *);
extern media_status_t AImage_getWidth(const AImage *, int32_t *);
extern media_status_t AImage_getHeight(const AImage *, int32_t *);
extern media_status_t AImage_getNumberOfPlanes(const AImage *, int32_t *);
extern media_status_t AImage_getPlaneRowStride(const AImage *, int, int32_t *);
extern media_status_t AImage_getPlanePixelStride(const AImage *, int, int32_t *);
extern media_status_t AImage_getPlaneData(const AImage *, int, uint8_t **, int *);
extern void ANativeWindow_acquire(ANativeWindow *);
extern void ANativeWindow_release(ANativeWindow *);

/* ---- capture ----------------------------------------------------------------------------- */
static volatile int g_images;

static void on_disconnected(void *c, ACameraDevice *d) { printf("camera disconnected\n"); }
static void on_error(void *c, ACameraDevice *d, int e) { printf("camera error %d\n", e); }
static void on_session(void *c, ACameraCaptureSession *s) { }
static void on_image(void *c, AImageReader *r) { g_images++; }

static int save_image(AImage *img, const char *out)
{
	int32_t w = 0, h = 0, planes = 0;
	AImage_getWidth(img, &w);
	AImage_getHeight(img, &h);
	AImage_getNumberOfPlanes(img, &planes);
	char path[512];
	snprintf(path, sizeof(path), "%s.yuv", out);
	FILE *f = fopen(path, "wb");
	snprintf(path, sizeof(path), "%s.txt", out);
	FILE *t = fopen(path, "w");
	if (!f || !t) {
		printf("cannot open %s.yuv/.txt\n", out);
		return 1;
	}
	fprintf(t, "format I420\nwidth %d\nheight %d\nplanes %d\n", w, h, planes);
	/* write Y full-res, then U and V at half-res, dropping row/pixel stride padding */
	for (int p = 0; p < planes && p < 3; p++) {
		uint8_t *data = NULL;
		int len = 0;
		int32_t rstride = 0, pstride = 0;
		AImage_getPlaneData(img, p, &data, &len);
		AImage_getPlaneRowStride(img, p, &rstride);
		AImage_getPlanePixelStride(img, p, &pstride);
		int pw = p == 0 ? w : w / 2, ph = p == 0 ? h : h / 2;
		fprintf(t, "plane%d len %d rowStride %d pixelStride %d -> %dx%d\n", p, len, rstride, pstride, pw, ph);
		for (int y = 0; y < ph; y++) {
			uint8_t *row = data + (size_t)y * rstride;
			if (pstride == 1) {
				fwrite(row, 1, pw, f);
			} else {
				for (int x = 0; x < pw; x++)
					fputc(row[x * pstride], f);
			}
		}
	}
	fclose(f);
	fclose(t);
	printf("saved %s.yuv (%dx%d I420)\n", out, w, h);
	return 0;
}

static int cmd_list(ACameraManager *mgr)
{
	ACameraIdList *ids = NULL;
	if (ACameraManager_getCameraIdList(mgr, &ids) != ACAMERA_OK || !ids) {
		printf("getCameraIdList failed\n");
		return 1;
	}
	printf("%d camera(s)\n", ids->numCameras);
	for (int i = 0; i < ids->numCameras; i++)
		printf("  id %s\n", ids->cameraIds[i]);
	ACameraManager_deleteCameraIdList(ids);
	return 0;
}

static int cmd_shot(ACameraManager *mgr, const char *id, const char *size, const char *out, int frames)
{
	int w, h;
	if (sscanf(size, "%dx%d", &w, &h) != 2)
		return 2;

	AImageReader *reader = NULL;
	if (AImageReader_new(w, h, FMT_YUV_420_888, 4, &reader) != AMEDIA_OK) {
		printf("AImageReader_new failed\n");
		return 1;
	}
	ANativeWindow *win = NULL;
	AImageReader_getWindow(reader, &win);
	ANativeWindow_acquire(win);
	AImageReader_ImageListener il = { NULL, on_image };
	AImageReader_setImageListener(reader, &il);

	ACameraDevice_StateCallbacks devcb = { NULL, on_disconnected, on_error };
	ACameraDevice *dev = NULL;
	camera_status_t cs = ACameraManager_openCamera(mgr, id, &devcb, &dev);
	if (cs != ACAMERA_OK || !dev) {
		printf("openCamera %s failed (%d)\n", id, cs);
		return 1;
	}
	printf("camera %s open\n", id);

	ACaptureSessionOutputContainer *cont = NULL;
	ACaptureSessionOutput *outp = NULL;
	ACaptureSessionOutputContainer_create(&cont);
	ACaptureSessionOutput_create(win, &outp);
	ACaptureSessionOutputContainer_add(cont, outp);
	ACameraCaptureSession_stateCallbacks scb = { NULL, on_session, on_session, on_session };
	ACameraCaptureSession *session = NULL;
	cs = ACameraDevice_createCaptureSession(dev, cont, &scb, &session);
	if (cs != ACAMERA_OK || !session) {
		printf("createCaptureSession failed (%d)\n", cs);
		return 1;
	}

	ACaptureRequest *req = NULL;
	ACameraOutputTarget *target = NULL;
	ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &req);
	ACameraOutputTarget_create(win, &target);
	ACaptureRequest_addTarget(req, target);
	int seq = 0;
	cs = ACameraCaptureSession_setRepeatingRequest(session, NULL, 1, &req, &seq);
	if (cs != ACAMERA_OK) {
		printf("setRepeatingRequest failed (%d)\n", cs);
		return 1;
	}
	printf("streaming, waiting for %d frames...\n", frames);

	int saved = 0, last = 0;
	for (int i = 0; i < 200 && !saved; i++) {
		usleep(100000);
		if (g_images != last) {
			last = g_images;
			if (last == 1 || last % 10 == 0)
				printf("%d frames\n", last);
		}
		if (g_images >= frames) {
			AImage *img = NULL;
			if (AImageReader_acquireLatestImage(reader, &img) == AMEDIA_OK && img) {
				saved = (save_image(img, out) == 0);
				AImage_delete(img);
			}
		}
	}

	ACameraCaptureSession_stopRepeating(session);
	ACameraCaptureSession_close(session);
	ACaptureSessionOutputContainer_free(cont);
	ACameraDevice_close(dev);
	ANativeWindow_release(win);
	AImageReader_delete(reader);
	printf("%s (%d frames seen)\n", saved ? "done" : "no frame saved", g_images);
	return saved ? 0 : 1;
}

int realmain(int argc, char **argv, char **env)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		printf("usage: ndkcam list | shot ID WxH OUT [FRAMES]\n");
		return 2;
	}
	ACameraManager *mgr = ACameraManager_create();
	if (!mgr) {
		printf("ACameraManager_create failed\n");
		return 1;
	}
	int rc = 2;
	if (!strcmp(argv[1], "list"))
		rc = cmd_list(mgr);
	else if (!strcmp(argv[1], "shot") && argc >= 5)
		rc = cmd_shot(mgr, argv[2], argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 20);
	else
		printf("usage: ndkcam list | shot ID WxH OUT [FRAMES]\n");
	ACameraManager_delete(mgr);
	return rc;
}

struct structors { void **pre, **init, **fini; };
struct structors __attribute__((used)) S;
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), struct structors *);
__asm__(
".global _start\n"
"_start:\n"
"  mov x0, sp\n"
"  mov x1, #0\n"
"  adrp x2, realmain\n  add x2, x2, :lo12:realmain\n"
"  adrp x3, S\n  add x3, x3, :lo12:S\n"
"  b __libc_init\n");
