// Galaxy S24 (SM-S921B) camera client for postmarketOS (Claude, 2026-11-15).
//
// Talks AIDL, through libgbinder, to Samsung's camera HAL running in the Android container
// (android-ctr0.sh start camera; s24-camd running for its sensor-service dependency):
//   android.hardware.camera.provider.ICameraProvider/internal/0   (frozen V2)
//   android.hardware.camera.device.ICameraDevice / ICameraDeviceSession / ICameraDeviceCallback
//   android.hardware.graphics.allocator.IAllocator/default        (frozen V2: image buffers)
// Transaction codes are declaration order in the frozen aidl_api dumps (pmos-s24\aidl-camera).
//
//   s24-cam list                          cameras: facing, orientation, YUV sizes
//   s24-cam shot ID WxH OUT [FRAMES]      stream FRAMES (default 30) YUV frames, save the last
//                                         one as OUT.yuv plus OUT.txt (layout) for conversion
//   s24-cam stream ID WxH                 endless raw NV12 on stdout (status on stderr); env:
//                                         S24CAM_ROT 0|90|180|270, S24CAM_HFLIP, S24CAM_DEPTH
//                                         (requests in flight), S24CAM_FPS "min,max" (def 30,30),
//                                         S24CAM_AF (AF mode, default 4 continuous-picture)
//   s24-cam serve ID[,ID..] WxH           s24-cam IS the PipeWire camera: one Video/Source node
//                                         per ID ("Back Camera", "Front Camera", marked with
//                                         api.libcamera.location), each a graph driver handing
//                                         over only the newest frame, upright from the sensor's
//                                         mounting angle. A camera runs only while an app
//                                         streams it (closed S24CAM_IDLE_CLOSE s, default 5,
//                                         after the app leaves); switching closes one, opens
//                                         the other. Needs the session's XDG_RUNTIME_DIR
//   more env: S24CAM_FACE=0 (front camera without face-priority 3A, on by default),
//   S24CAM_STATS (status every second), S24CAM_COLOR (frame colour + white balance in it),
//   S24CAM_TEMPLATE=1|alt (request = the camera's own template + ours; alt = A/B per open),
//   S24CAM_DUMPTPL (print the template), S24CAM_FACETEST=N (face priority off/on every N frames)
//
// Build: gcc -O2 -Wall -DS24CAM_PIPEWIRE -o s24-cam s24-cam.c
//          $(pkg-config --cflags --libs libgbinder glib-2.0 libpipewire-0.3)
//        (drop -DS24CAM_PIPEWIRE and libpipewire-0.3 for a pipe-only build)
#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>
#include <gbinder.h>
#include <glib.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef S24CAM_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#endif

#define AIDL_GET_VERSION 0x00ffffff
#define AIDL_GET_HASH 0x00fffffe

#define PROVIDER_IFACE "android.hardware.camera.provider.ICameraProvider"
#define PROVIDER_CB_IFACE "android.hardware.camera.provider.ICameraProviderCallback"
#define DEVICE_IFACE "android.hardware.camera.device.ICameraDevice"
#define SESSION_IFACE "android.hardware.camera.device.ICameraDeviceSession"
#define CALLBACK_IFACE "android.hardware.camera.device.ICameraDeviceCallback"
#define DEVICE_HASH "c03791dc9ced356c793cc487d0de3266dcb06ac9"
#define ALLOC_IFACE "android.hardware.graphics.allocator.IAllocator"

enum { PROV_SET_CALLBACK = 1, PROV_GET_VENDOR_TAGS, PROV_GET_CAMERA_ID_LIST,
       PROV_GET_CAMERA_DEVICE_INTERFACE };
enum { DEV_GET_CHARACTERISTICS = 1, DEV_GET_PHYSICAL_CHARACTERISTICS, DEV_GET_RESOURCE_COST,
       DEV_IS_STREAM_COMBINATION_SUPPORTED, DEV_OPEN };
enum { SES_CLOSE = 1, SES_CONFIGURE_STREAMS, SES_CONSTRUCT_DEFAULT_REQUEST_SETTINGS, SES_FLUSH,
       SES_GET_REQUEST_FMQ, SES_GET_RESULT_FMQ, SES_IS_RECONFIGURATION_REQUIRED,
       SES_PROCESS_CAPTURE_REQUEST };
enum { CB_NOTIFY = 1, CB_PROCESS_CAPTURE_RESULT, CB_REQUEST_STREAM_BUFFERS,
       CB_RETURN_STREAM_BUFFERS };
enum { ALLOC_ALLOCATE = 1, ALLOC_ALLOCATE2, ALLOC_IS_SUPPORTED };

/* android.hardware.graphics.common */
#define PIXEL_FORMAT_YCBCR_420_888 0x23
#define PIXEL_FORMAT_IMPLEMENTATION_DEFINED 0x22
#define USAGE_CPU_READ_OFTEN 0x3ULL
#define USAGE_CAMERA_OUTPUT (1ULL << 17)
/* camera metadata (system/media camera_metadata_tags.h) */
#define TAG_LENS_FACING 0x080005
#define TAG_SENSOR_ORIENTATION 0x0e000e
#define TAG_SCALER_AVAILABLE_STREAM_CONFIGURATIONS 0x0d000a
#define TAG_INFO_SUPPORTED_HARDWARE_LEVEL 0x150000
#define TAG_INFO_SUPPORTED_BUFFER_MANAGEMENT_VERSION 0x150002
#define TAG_REQUEST_PARTIAL_RESULT_COUNT 0x0c000b
#define TAG_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS 0x1a0000

static GBinderServiceManager *sm;
static GMainLoop *loop;
static int stream_mode = 0;
static int g_depth = -1;                 /* stream mode: max requests in flight */
static double st_submit[64];             /* submit time per frameNumber%64 */
static double st_lat_sum, st_proc_sum, st_t0;
static int st_n;
/* latest 3A state from the result metadata (shown in the stream status line) */
static int g_af_state = -1, g_lens_state = -1, g_ae_state = -1;
static float g_focus = -1;
static gint64 g_exposure = -1;
static gint32 g_iso = -1, g_af_pos = -1;
/* white balance as the HAL applied it (results): AWB mode/state, colour-correction mode + gains */
static int g_awb_mode = -1, g_awb_state = -1, g_cc_mode = -1;
static float g_wb[4] = { -1, -1, -1, -1 }, g_ccm[9];
static int g_meta_inline, g_meta_fmq;
static gint64 st_shutter[64];            /* sensor start-of-exposure (ns) per frameNumber%64 */
static double st_sens_sum;               /* exposure start -> frame in hand, summed */
static int st_sens_n, st_drop;
static int pw_mode;                      /* serve: frames go straight to a PipeWire Video/Source */

/* the cameras this process serves (serve mode: one PipeWire node each, one open at a time;
 * shot/stream: just one) */
typedef struct {
	const char *id;
	GBinderClient *dev;
	int facing, orient;     /* LENS_FACING (0 front, 1 back, 2 external), SENSOR_ORIENTATION */
	int rot, hflip;         /* applied to every frame: clockwise rotation, then a mirror */
	int active;             /* serve: an app is streaming from this camera's node */
	char node[32], desc[32];
#ifdef S24CAM_PIPEWIRE
	struct pw_stream *pws;
	struct spa_hook hook;
#endif
} Cam;
#define MAXCAMS 4
static Cam cams[MAXCAMS];
static int ncams;
static Cam *cur;                         /* the camera in use; its session is cap.session */
static Cam *want;                        /* serve: switch to it once in-flight frames are back */
static double clock_ms(clockid_t id)
{
	struct timespec t;
	clock_gettime(id, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static const char *af_state_name(int s)
{
	static const char *n[] = { "inactive", "scanning", "focused", "active-scan", "locked",
	                           "not-focused-locked", "unfocused" };
	return s >= 0 && s < 7 ? n[s] : "?";
}
static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---------- camera metadata (camera_metadata_t, read only) ------------------------------- */
typedef struct {
	uint32_t size, version, flags, entry_count, entry_capacity, entries_start, data_count,
		data_capacity, data_start, padding;
	uint64_t vendor_id;
} MetaHeader;
typedef struct { uint32_t tag, count, data; uint8_t type, reserved[3]; } MetaEntry;
static const int meta_type_size[] = { 1, 4, 4, 8, 8, 8 };

/* pointer to the data of TAG (and its count), or NULL */
static const void *meta_find(const uint8_t *m, size_t len, uint32_t tag, uint32_t *count, int *type)
{
	if (!m || len < sizeof(MetaHeader))
		return NULL;
	const MetaHeader *h = (const MetaHeader *)m;
	if (h->entries_start + (size_t)h->entry_count * sizeof(MetaEntry) > len)
		return NULL;
	const MetaEntry *e = (const MetaEntry *)(m + h->entries_start);
	for (uint32_t i = 0; i < h->entry_count; i++) {
		if (e[i].tag != tag)
			continue;
		size_t bytes = (size_t)e[i].count * meta_type_size[e[i].type < 6 ? e[i].type : 0];
		*count = e[i].count;
		if (type)
			*type = e[i].type;
		if (bytes <= 4)
			return &e[i].data;
		if (h->data_start + e[i].data + bytes > len)
			return NULL;
		return m + h->data_start + e[i].data;
	}
	return NULL;
}

/* ---------- build a camera_metadata request (the HAL's default template is a 1-entry skeleton;
 * the Android framework normally fills the rest, so do the minimum the HAL requires) ---------- */
#define TAG_CONTROL_AE_MODE 0x010003
#define TAG_CONTROL_AE_TARGET_FPS_RANGE 0x010005
#define TAG_CONTROL_AF_MODE 0x010007
#define TAG_CONTROL_AF_TRIGGER 0x010009
#define TAG_CONTROL_AWB_MODE 0x01000b
#define TAG_CONTROL_AF_AVAILABLE_MODES 0x010017
#define TAG_CONTROL_AE_STATE 0x01001f
#define TAG_CONTROL_AF_STATE 0x010020
#define TAG_LENS_FOCUS_DISTANCE 0x080003           /* float, diopters (0 = infinity) */
#define TAG_LENS_STATE 0x080009                    /* 0 stationary, 1 moving */
#define TAG_LENS_INFO_MINIMUM_FOCUS_DISTANCE 0x090005
#define TAG_SENSOR_EXPOSURE_TIME 0x0e0000          /* int64 ns */
#define TAG_SENSOR_SENSITIVITY 0x0e0002            /* int32 ISO (0x0e0001 is FRAME_DURATION) */
#define TAG_SS_AF_POS_CUR 0x80130002               /* Samsung vendor: actuator position now */
#define TAG_CONTROL_CAPTURE_INTENT 0x01000d
#define TAG_CONTROL_MODE 0x01000f
#define TAG_CONTROL_ZOOM_RATIO 0x01003b            /* float; CONTROL_ZOOM_RATIO (index 0x3b) */
#define TAG_SCALER_CROP_REGION 0x0d0000            /* int32[4] x,y,w,h in active-array coords */
#define TAG_SENSOR_INFO_ACTIVE_ARRAY_SIZE 0x0f0000 /* int32[4] x,y,w,h */
#define TAG_COLOR_CORRECTION_MODE 0x000000         /* 0 matrix, 1 fast, 2 high quality */
#define TAG_COLOR_CORRECTION_TRANSFORM 0x000001    /* rational[9] */
#define TAG_COLOR_CORRECTION_GAINS 0x000002        /* float[4] R, G-even, G-odd, B */
#define TAG_CONTROL_AWB_STATE 0x010022             /* 0 inactive, 1 searching, 2 converged, 3 locked */
#define TAG_CONTROL_SCENE_MODE 0x010010            /* 1 FACE_PRIORITY (needs CONTROL_MODE 2) */
#define TAG_CONTROL_AVAILABLE_SCENE_MODES 0x010019
#define TAG_STATISTICS_FACE_DETECT_MODE 0x110000   /* 0 off, 1 simple, 2 full */
#define TAG_STATISTICS_FACE_SCORES 0x110007        /* byte per detected face */
#define TAG_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES 0x120000
#define TAG_STATISTICS_INFO_MAX_FACE_COUNT 0x120002
/* front camera: face-priority 3A (exposure and white balance weighted on the faces it finds),
 * env S24CAM_FACE=0|1 */
static int g_face = -1;
static int face_priority(void)
{
	if (g_face < 0)
		g_face = getenv("S24CAM_FACE") ? atoi(getenv("S24CAM_FACE")) != 0 : 1;
	return g_face && cur && cur->facing == 0;
}
static int g_ctl_mode = -1, g_scene = -1, g_fd_mode = -1, g_faces = -1;
/* set from the environment (S24CAM_ZOOM) + the device's active array, to steer a logical camera to
 * a non-primary physical sensor (Tele/UW), whose stream controller doesn't need the null m+464 */
static float g_zoom = 1.0f;
static gint32 g_crop[4];       /* valid when g_crop[2] > 0 */
enum { CM_BYTE = 0, CM_INT32 = 1, CM_FLOAT = 2, CM_INT64 = 3 };
static const int cm_type_size[] = { 1, 4, 4, 8, 8, 8 };

struct cm_entry { guint32 tag, count; guint8 type; const void *data; };

static guint8 *build_metadata(const struct cm_entry *e, int n, gsize *outlen)
{
	/* entries must be sorted by tag; header 48, then 16-byte entries, then 8-aligned data */
	gsize entries_start = sizeof(MetaHeader);
	gsize entries_bytes = (gsize)n * sizeof(MetaEntry);
	gsize data_start = (entries_start + entries_bytes + 7) & ~(gsize)7;
	gsize data = 0;
	for (int i = 0; i < n; i++) {
		gsize bytes = (gsize)e[i].count * cm_type_size[e[i].type];
		if (bytes > 4)
			data += (bytes + 7) & ~(gsize)7;
	}
	gsize total = data_start + data;
	guint8 *m = g_malloc0(total);
	MetaHeader *h = (MetaHeader *)m;
	h->size = total;
	h->version = 1;
	h->entry_count = h->entry_capacity = n;
	h->entries_start = entries_start;
	h->data_count = h->data_capacity = data;
	h->data_start = data_start;
	h->vendor_id = ~0ULL;
	MetaEntry *me = (MetaEntry *)(m + entries_start);
	gsize doff = 0;
	for (int i = 0; i < n; i++) {
		gsize bytes = (gsize)e[i].count * cm_type_size[e[i].type];
		me[i].tag = e[i].tag;
		me[i].count = e[i].count;
		me[i].type = e[i].type;
		if (bytes <= 4) {
			memcpy(&me[i].data, e[i].data, bytes);
		} else {
			me[i].data = doff;
			memcpy(m + data_start + doff, e[i].data, bytes);
			doff += (bytes + 7) & ~(gsize)7;
		}
	}
	*outlen = total;
	return m;
}

/* the request settings we choose ourselves (values in statics: the entries point at them) */
static int request_entries(struct cm_entry *e)
{
	static guint8 ae, af, awb, intent, scene, fd, mode;     /* CONTROL_MODE is a byte enum */
	static gint32 fps[2];
	static float zoom;
	ae = 1; af = 0; awb = 1; intent = 1; mode = 1; fps[0] = 15; fps[1] = 30;
	/* front camera: CONTROL_MODE USE_SCENE_MODE + SCENE_MODE FACE_PRIORITY + face detection, so
	 * exposure and white balance follow the faces (what selfie cameras do) */
	int face = face_priority();
	if (face) { mode = 2; scene = 1; fd = 1; }
	/* live preview: lock 30 fps so dim light doesn't stretch exposure down to 15 fps, and focus
	 * continuously (AF_MODE 4 CONTINUOUS_PICTURE: fast passive scans, what Android's camera app
	 * previews with; 3 CONTINUOUS_VIDEO is slower and smoother; 0 OFF leaves the lens parked) */
	if (stream_mode) { fps[0] = 30; fps[1] = 30; af = 4; }
	{ const char *fe = getenv("S24CAM_FPS"); if (fe) sscanf(fe, "%d,%d", &fps[0], &fps[1]); }
	{ const char *a = getenv("S24CAM_AF"); if (a) af = (guint8)atoi(a); }
	zoom = g_zoom;
	/* tags MUST stay sorted ascending: CONTROL (0x01xxxx) then SCALER (0x0dxxxx) */
	int n = 0;
	e[n++] = (struct cm_entry){ TAG_CONTROL_AE_MODE, 1, CM_BYTE, &ae };
	e[n++] = (struct cm_entry){ TAG_CONTROL_AE_TARGET_FPS_RANGE, 2, CM_INT32, fps };
	e[n++] = (struct cm_entry){ TAG_CONTROL_AF_MODE, 1, CM_BYTE, &af };
	e[n++] = (struct cm_entry){ TAG_CONTROL_AWB_MODE, 1, CM_BYTE, &awb };
	e[n++] = (struct cm_entry){ TAG_CONTROL_CAPTURE_INTENT, 1, CM_BYTE, &intent };
	e[n++] = (struct cm_entry){ TAG_CONTROL_MODE, 1, CM_BYTE, &mode };
	if (face)
		e[n++] = (struct cm_entry){ TAG_CONTROL_SCENE_MODE, 1, CM_BYTE, &scene };
	if (g_zoom > 1.01f)
		e[n++] = (struct cm_entry){ TAG_CONTROL_ZOOM_RATIO, 1, CM_FLOAT, &zoom };
	if (g_crop[2] > 0)
		e[n++] = (struct cm_entry){ TAG_SCALER_CROP_REGION, 4, CM_INT32, g_crop };
	if (face)
		e[n++] = (struct cm_entry){ TAG_STATISTICS_FACE_DETECT_MODE, 1, CM_BYTE, &fd };
	return n;
}

static int cm_cmp(const void *a, const void *b)
{
	guint32 x = ((const struct cm_entry *)a)->tag, y = ((const struct cm_entry *)b)->tag;
	return x < y ? -1 : x > y;
}

static guint8 *default_request(gsize *outlen)
{
	struct cm_entry e[16];
	int n = request_entries(e);
	qsort(e, n, sizeof(*e), cm_cmp);
	return build_metadata(e, n, outlen);
}

/* print a camera_metadata blob, one entry per line (S24CAM_DUMPTPL) */
static void dump_metadata(const char *what, const guint8 *m, gsize len)
{
	const MetaHeader *h = (const MetaHeader *)m;
	if (!m || len < sizeof(MetaHeader) || h->entries_start + (gsize)h->entry_count * sizeof(MetaEntry) > len)
		return;
	const MetaEntry *te = (const MetaEntry *)(m + h->entries_start);
	fprintf(stderr, "%s: %u entries, vendor id 0x%llx\n", what, h->entry_count, (unsigned long long)h->vendor_id);
	for (guint32 i = 0; i < h->entry_count; i++) {
		int t = te[i].type < 6 ? te[i].type : 0;
		gsize bytes = (gsize)te[i].count * cm_type_size[t];
		const guint8 *d = bytes <= 4 ? (const guint8 *)&te[i].data : m + h->data_start + te[i].data;
		if (bytes > 4 && h->data_start + te[i].data + bytes > len)
			continue;
		fprintf(stderr, "  tpl 0x%08x t%d n%u:", te[i].tag, te[i].type, te[i].count);
		for (guint32 k = 0; k < te[i].count && k < 9; k++) {
			switch (t) {
			case CM_BYTE: fprintf(stderr, " %u", d[k]); break;
			case CM_INT32: fprintf(stderr, " %d", ((const gint32 *)d)[k]); break;
			case CM_FLOAT: fprintf(stderr, " %g", ((const float *)d)[k]); break;
			case CM_INT64: fprintf(stderr, " %lld", (long long)((const gint64 *)d)[k]); break;
			case 4: fprintf(stderr, " %g", ((const double *)d)[k]); break;
			case 5: fprintf(stderr, " %d/%d", ((const gint32 *)d)[2 * k], ((const gint32 *)d)[2 * k + 1]); break;
			}
		}
		fprintf(stderr, "\n");
	}
}

/* the HAL's own template for this camera with our settings on top: what an Android app gets
 * from CaptureRequest.Builder(template) + set(). The template carries the camera's tuning
 * choices (noise reduction, shading, colour correction, Samsung's vendor settings, ...) */
static guint8 *template_request(const guint8 *t, gsize tlen, gsize *outlen)
{
	const MetaHeader *h = (const MetaHeader *)t;
	if (!t || tlen < sizeof(MetaHeader) || !h->entry_count ||
	    h->entries_start + (gsize)h->entry_count * sizeof(MetaEntry) > tlen)
		return NULL;
	const MetaEntry *te = (const MetaEntry *)(t + h->entries_start);
	struct cm_entry *e = g_new0(struct cm_entry, h->entry_count + 16);
	int n = request_entries(e), ours = n;
	for (guint32 i = 0; i < h->entry_count; i++) {
		int dup = 0;
		for (int k = 0; k < ours; k++)
			dup |= e[k].tag == te[i].tag;
		if (dup || te[i].type > 5)
			continue;
		gsize bytes = (gsize)te[i].count * cm_type_size[te[i].type];
		if (bytes > 4 && h->data_start + te[i].data + bytes > tlen)
			continue;
		const void *d = bytes <= 4 ? (const void *)&te[i].data : t + h->data_start + te[i].data;
		e[n++] = (struct cm_entry){ te[i].tag, te[i].count, te[i].type, d };
	}
	qsort(e, n, sizeof(*e), cm_cmp);
	guint8 *m = build_metadata(e, n, outlen);
	((MetaHeader *)m)->vendor_id = h->vendor_id;
	g_free(e);
	return m;
}

/* ---------- parcel helpers --------------------------------------------------------------- */
static void write_metadata(GBinderWriter *w, const void *blob, int len)     /* CameraMetadata */
{
	GBinderWriter p;
	gbinder_writer_start_parcelable(w, &p);
	if (len > 0)
		gbinder_writer_append_byte_array(&p, blob, len);
	else
		gbinder_writer_append_int32(&p, 0);     /* empty, not null (-1): non-nullable byte[] */
	gbinder_writer_finish_parcelable(&p);
}

static void write_pfd(GBinderWriter *w, int fd)            /* ParcelFileDescriptor, non-null */
{
	gbinder_writer_append_int32(w, 1);
	gbinder_writer_append_int32(w, 0);      /* no comm channel */
	gbinder_writer_append_fd(w, fd);
}

typedef struct { int nfds, nints; int fds[16]; int ints[384]; } Handle;

static void write_handle(GBinderWriter *w, const Handle *h)    /* NativeHandle */
{
	GBinderWriter p;
	gbinder_writer_start_parcelable(w, &p);
	gbinder_writer_append_int32(&p, h ? h->nfds : 0);
	for (int i = 0; h && i < h->nfds; i++)
		write_pfd(&p, h->fds[i]);
	gbinder_writer_append_int32(&p, h ? h->nints : 0);
	for (int i = 0; h && i < h->nints; i++)
		gbinder_writer_append_int32(&p, h->ints[i]);
	gbinder_writer_finish_parcelable(&p);
}

static gboolean read_handle(GBinderReader *r, Handle *h)      /* NativeHandle, fds dup'ed */
{
	GBinderReader p;
	gboolean nn;
	gint32 n;
	memset(h, 0, sizeof(*h));
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return FALSE;
	gbinder_reader_read_int32(&p, &n);
	for (int i = 0; i < n; i++) {
		gint32 nonnull = 0, comm = 0;
		gbinder_reader_read_int32(&p, &nonnull);
		if (!nonnull)
			continue;
		gbinder_reader_read_int32(&p, &comm);
		int fd = gbinder_reader_read_dup_fd(&p);
		if (comm)
			close(gbinder_reader_read_dup_fd(&p));
		if (h->nfds < 16)
			h->fds[h->nfds++] = fd;
		else
			close(fd);
	}
	gbinder_reader_read_int32(&p, &n);
	for (int i = 0; i < n; i++) {
		gint32 v = 0;
		gbinder_reader_read_int32(&p, &v);
		if (h->nints < 384)
			h->ints[h->nints++] = v;
	}
	gbinder_reader_finish_parcelable(&p);
	return TRUE;
}

static void close_handle(Handle *h)
{
	for (int i = 0; i < h->nfds; i++)
		close(h->fds[i]);
	h->nfds = 0;
}

static guint8 *read_metadata(GBinderReader *r, gsize *len)    /* CameraMetadata, copied */
{
	GBinderReader p;
	gboolean nn;
	gsize n = 0;
	*len = 0;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return NULL;
	const void *b = gbinder_reader_read_byte_array(&p, &n);
	guint8 *copy = n ? g_memdup2(b, n) : NULL;
	gbinder_reader_finish_parcelable(&p);
	*len = n;
	return copy;
}

/* status word of an AIDL reply; prints exceptions */
static gboolean reply_ok(GBinderRemoteReply *rep, int status, const char *what, GBinderReader *r)
{
	if (!rep || status) {
		fprintf(stderr, "s24-cam: %s: transaction failed (%d)\n", what, status);
		return FALSE;
	}
	gint32 ex = 0;
	gbinder_remote_reply_init_reader(rep, r);
	gbinder_reader_read_int32(r, &ex);
	if (ex) {
		char *msg = gbinder_reader_read_string16(r);
		gint32 hdr = 0, code = 0;
		gbinder_reader_read_int32(r, &hdr);
		gbinder_reader_read_int32(r, &code);
		fprintf(stderr, "s24-cam: %s: exception %d (%s) code %d\n", what, ex, msg ? msg : "", code);
		g_free(msg);
		return FALSE;
	}
	return TRUE;
}

static GBinderClient *service(const char *name, const char *iface)
{
	int st = 0;
	GBinderRemoteObject *o = gbinder_servicemanager_get_service_sync(sm, name, &st);
	if (!o) {
		fprintf(stderr, "s24-cam: no service %s (%d)\n", name, st);
		return NULL;
	}
	return gbinder_client_new(o, iface);
}

/* ---------- provider / device ----------------------------------------------------------- */
static GBinderClient *provider;

/* cameraserver calls getVendorTags on the provider at startup; the HAL may build/cache its
 * process-global VendorTagDescriptor only then, which the full default template + CameraIdManager
 * need. Call it once so the HAL initialises its metadata system even without cameraserver. */
/* minimal ICameraProviderCallback: the 3 status callbacks are void; just ack everything. The HAL
 * may only build its full session state once a provider callback is registered (setCallback). */
static GBinderLocalReply *provcb_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                          guint code, guint flags, int *status, void *user)
{
	GBinderLocalReply *rep = gbinder_local_object_new_reply(obj);
	GBinderWriter w;
	gbinder_local_reply_init_writer(rep, &w);
	*status = 0;
	if (code == AIDL_GET_VERSION) {
		gbinder_writer_append_int32(&w, 0);
		gbinder_writer_append_int32(&w, 2);
		return rep;
	}
	if (code == AIDL_GET_HASH) {
		gbinder_writer_append_int32(&w, 0);
		gbinder_writer_append_string16(&w, "notfrozen");
		return rep;
	}
	gbinder_writer_append_int32(&w, 0);     /* EX_NONE for the void status callbacks */
	return rep;
}

static void set_provider_callback(void)
{
	GBinderLocalObject *cb = gbinder_servicemanager_new_local_object(sm, PROVIDER_CB_IFACE, provcb_transact, NULL);
	gbinder_local_object_set_stability(cb, GBINDER_STABILITY_VINTF);
	GBinderLocalRequest *req = gbinder_client_new_request(provider);
	GBinderWriter w;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_local_object(&w, cb);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(provider, PROV_SET_CALLBACK, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	if (reply_ok(rep, st, "setCallback", &r))
		fprintf(stderr, "setCallback: ok\n");
	if (rep)
		gbinder_remote_reply_unref(rep);
}

static void prime_vendor_tags(void)
{
	GBinderLocalRequest *req = gbinder_client_new_request(provider);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(provider, PROV_GET_VENDOR_TAGS, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	if (reply_ok(rep, st, "getVendorTags", &r)) {
		gint32 nsec = 0;
		gbinder_reader_read_int32(&r, &nsec);
		fprintf(stderr, "getVendorTags: %d vendor section(s)\n", nsec);
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
}

static char **camera_ids(int *n)
{
	GBinderLocalRequest *req = gbinder_client_new_request(provider);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(provider, PROV_GET_CAMERA_ID_LIST, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	char **ids = NULL;
	*n = 0;
	if (reply_ok(rep, st, "getCameraIdList", &r)) {
		gint32 c = 0;
		gbinder_reader_read_int32(&r, &c);
		ids = g_new0(char *, c + 1);
		for (int i = 0; i < c; i++)
			ids[i] = gbinder_reader_read_string16(&r);
		*n = c;
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	return ids;
}

static GBinderClient *camera_device(const char *id)
{
	GBinderLocalRequest *req = gbinder_client_new_request(provider);
	GBinderWriter w;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_string16(&w, id);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(provider, PROV_GET_CAMERA_DEVICE_INTERFACE, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	GBinderClient *c = NULL;
	if (reply_ok(rep, st, "getCameraDeviceInterface", &r)) {
		GBinderRemoteObject *o = gbinder_reader_read_object(&r);
		if (o) {
			c = gbinder_client_new(o, DEVICE_IFACE);
			gbinder_remote_object_unref(o);
		}
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	return c;
}

static guint8 *characteristics(GBinderClient *dev, gsize *len)
{
	GBinderLocalRequest *req = gbinder_client_new_request(dev);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(dev, DEV_GET_CHARACTERISTICS, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	guint8 *m = NULL;
	*len = 0;
	if (reply_ok(rep, st, "getCameraCharacteristics", &r))
		m = read_metadata(&r, len);
	if (rep)
		gbinder_remote_reply_unref(rep);
	return m;
}

static int cmd_list(void)
{
	int n;
	char **ids = camera_ids(&n);
	if (!ids)
		return 1;
	fprintf(stderr, "%d camera(s)\n", n);
	for (int i = 0; i < n; i++) {
		GBinderClient *dev = camera_device(ids[i]);
		gsize len = 0;
		guint8 *m = dev ? characteristics(dev, &len) : NULL;
		uint32_t c;
		const guint8 *facing = meta_find(m, len, TAG_LENS_FACING, &c, NULL);
		const gint32 *orient = meta_find(m, len, TAG_SENSOR_ORIENTATION, &c, NULL);
		const guint8 *level = meta_find(m, len, TAG_INFO_SUPPORTED_HARDWARE_LEVEL, &c, NULL);
		const guint8 *bufman = meta_find(m, len, TAG_INFO_SUPPORTED_BUFFER_MANAGEMENT_VERSION, &c, NULL);
		const gint32 *partial = meta_find(m, len, TAG_REQUEST_PARTIAL_RESULT_COUNT, &c, NULL);
		uint32_t pc = 0;
		const char *phys = meta_find(m, len, TAG_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &pc, NULL);
		static const char *fac[] = { "front", "back", "external" };
		fprintf(stderr, "%s: %s, orientation %d, hw level %d, metadata %zu bytes%s, partial results %d",
		       ids[i], facing && *facing < 3 ? fac[*facing] : "?", orient ? *orient : -1,
		       level ? *level : -1, len, bufman ? ", HAL buffer management" : "",
		       partial ? *partial : 1);
		if (phys) {
			fprintf(stderr, ", logical of:");
			for (uint32_t k = 0; k < pc; k += strlen(phys + k) + 1)
				fprintf(stderr, " %s", phys + k);
		}
		fprintf(stderr, "\n");
		const guint8 *afm = meta_find(m, len, TAG_CONTROL_AF_AVAILABLE_MODES, &c, NULL);
		fprintf(stderr, "  AF modes:");
		for (uint32_t k = 0; afm && k < c; k++)
			fprintf(stderr, " %d", afm[k]);
		const float *mfd = meta_find(m, len, TAG_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &c, NULL);
		if (mfd) {
			float f;
			memcpy(&f, mfd, sizeof(f));
			fprintf(stderr, " | min focus %.2f dpt (%.1f cm)", f, f > 0 ? 100.0f / f : 0.0f);
		}
		fprintf(stderr, "\n");
		const gint32 *sc = meta_find(m, len, TAG_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &c, NULL);
		fprintf(stderr, "  YUV out:");
		for (uint32_t k = 0; sc && k + 3 < c; k += 4)
			if (sc[k] == PIXEL_FORMAT_YCBCR_420_888 && sc[k + 3] == 0)
				fprintf(stderr, " %dx%d", sc[k + 1], sc[k + 2]);
		fprintf(stderr, "\n");
		g_free(m);
		if (dev)
			gbinder_client_unref(dev);
	}
	g_strfreev(ids);
	return 0;
}

/* ---------- capture ---------------------------------------------------------------------- */
#define MAXBUF 16
static struct {
	GBinderClient *session, *alloc;
	GBinderLocalObject *cb;
	int width, height, frames, format;
	guint64 usage, buf_usage;      /* what the stream asks for / what the buffers were made with */
	int nbuf, stride;
	Handle buf[MAXBUF];
	gboolean busy[MAXBUF], sent_handle[MAXBUF];
	int fbuf[64];               /* frameNumber%64 -> buffer index we sent (HAL returns bufferId 0) */
	guint8 *settings;
	gsize settings_len;
	gboolean resend;            /* settings changed mid-session: send them with the next request */
	int next_frame, done_frames, errors;
	const char *out;
	gboolean saved;
} cap;

static gboolean submit_next(gpointer unused);
static void frame_color(int b, const char *what);
#ifdef S24CAM_PIPEWIRE
static void cam_wanted(Cam *c);
static void cam_idle_soon(void);
#endif

static int inflight_count(void)
{
	int n = 0;
	for (int i = 0; i < cap.nbuf; i++)
		n += cap.busy[i] ? 1 : 0;
	return n;
}

/* safe to close: every buffer is back AND the HAL has been quiet for a moment. It still
 * delivers a frame's result metadata (and shutter messages) shortly after its buffer, and a
 * close that lands during such a delivery crashes Samsung's provider (2026-11-17: its result
 * thread called into the session the close was tearing down; init restarts it UNPRIMED) */
static double last_cb_ms;                   /* time of the HAL's latest callback to us */
static int settled(void)
{
	static double drained_at;               /* first seen with every buffer back (0: some out) */
	if (inflight_count()) {
		drained_at = 0;
		return 0;
	}
	double t = now_ms();
	if (!drained_at)
		drained_at = t;
	return t - last_cb_ms >= 150 || t - drained_at >= 1000;    /* quiet, or never waits >1 s */
}

/* stop asking for frames and leave the main loop once the HAL has handed back every buffer
 * (or after 3 s): closing a session with frames still in flight crashes Samsung's provider (its
 * result thread then locks a mutex the close destroyed; init restarts it UNPRIMED) */
static int quitting;
static gboolean drain_check(gpointer unused)
{
	static int ticks;
	if (settled() || ++ticks > 300) {
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}
static void quit_when_drained(void)
{
	if (!quitting++)
		g_timeout_add(10, drain_check, NULL);
}

/* serve: keep the phone awake (s24-powerd honours kernel wake locks) only while a camera is open */
static void wake_hold(int on)
{
	static int held;
	if (!pw_mode || on == held)
		return;
	int fd = open(on ? "/sys/power/wake_lock" : "/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, "s24-camera", 10) < 0)
			fprintf(stderr, "s24-cam: wake lock: %s\n", strerror(errno));
		close(fd);
	}
	held = on;
}

/* serve: frames asked for but none back for 5 s = a wedged HAL: exit so the supervisor redoes
 * the whole setup (an app would otherwise sit on a frozen picture) */
static double last_progress;
static int screen_off;                   /* serve: the panel is dark (cameras paused) */
#ifdef S24CAM_PIPEWIRE
/* serve: is the panel lit? 1 / 0, -1 if unknown. The same test as s24-powerd: a compositor that
 * switches the output off unlinks the connector ("disabled"); KWin's DPMS only blanks it, which the
 * display driver's state dump shows as the DSI-1 connector's "state: off" (or no crtc) */
static int screen_lit(void)
{
	static char conn[96], state[96];
	static int looked;
	char buf[32];
	if (!looked) {
		looked = 1;
		glob_t g;
		if (glob("/sys/class/drm/card*-DSI-1/enabled", 0, NULL, &g) == 0 && g.gl_pathc)
			snprintf(conn, sizeof(conn), "%s", g.gl_pathv[0]);
		globfree(&g);
		if (glob("/sys/kernel/debug/dri/*/name", 0, NULL, &g) == 0)
			for (size_t i = 0; i < g.gl_pathc; i++) {
				FILE *f = fopen(g.gl_pathv[i], "r");
				if (f && fgets(buf, sizeof(buf), f) && strstr(buf, "exynos")) {
					snprintf(state, sizeof(state), "%.*s/state",
					         (int)(strlen(g.gl_pathv[i]) - 5), g.gl_pathv[i]);
				}
				if (f)
					fclose(f);
			}
		globfree(&g);
	}
	FILE *f = conn[0] ? fopen(conn, "r") : NULL;
	if (f) {
		int dis = fgets(buf, sizeof(buf), f) && !strncmp(buf, "disabled", 8);
		fclose(f);
		if (dis)
			return 0;
	}
	if (!state[0] || !(f = fopen(state, "r")))
		return -1;
	int inside = 0, lit = -1;
	char ln[256];
	while (fgets(ln, sizeof(ln), f)) {
		ln[strcspn(ln, "\n")] = 0;
		if (ln[0] != ' ' && ln[0] != '\t') {
			size_t n = strlen(ln);
			inside = !strncmp(ln, "connector[", 10) && n > 7 && !strcmp(ln + n - 7, ": DSI-1");
			continue;
		}
		if (!inside)
			continue;
		char *s = ln + strspn(ln, " \t");
		if (!strncmp(s, "state:", 6))
			lit = strcmp(s + 6 + strspn(s + 6, " "), "off") != 0;
		else if (!strncmp(s, "crtc=", 5) && lit < 0)
			lit = strcmp(s + 5, "(null)") != 0;
	}
	fclose(f);
	return lit;
}

/* serve: with the screen dark, cameras pause (as Android's camera apps release theirs), so an
 * app left open in a pocket doesn't keep the camera running and the phone awake; they resume
 * when it lights again. Off with S24CAM_SCREEN_PAUSE=0 */
static void screen_check(void)
{
	static int dark, on = -1;
	if (on < 0)
		on = !getenv("S24CAM_SCREEN_PAUSE") || atoi(getenv("S24CAM_SCREEN_PAUSE"));
	if (!on)
		return;
	int lit = screen_lit();
	if (lit == 0 && !screen_off && ++dark >= 2) {
		screen_off = 1;
		fprintf(stderr, "screen off: cameras paused\n");
		if (cap.session)
			cam_idle_soon();
	} else if (lit == 1) {
		dark = 0;
		if (screen_off) {
			screen_off = 0;
			fprintf(stderr, "screen on: cameras resume\n");
			Cam *c = cur && cur->active ? cur : NULL;
			for (int i = 0; !c && i < ncams; i++)
				if (cams[i].active)
					c = &cams[i];
			if (c)
				cam_wanted(c);
		}
	}
}

static gboolean watchdog(gpointer unused)
{
	if (cap.session && inflight_count() > 0 && now_ms() - last_progress > 5000) {
		fprintf(stderr, "s24-cam: no frame from the camera for 5 s; exiting (needs a full setup again)\n");
		_exit(4);
	}
	screen_check();
	return G_SOURCE_CONTINUE;
}
#endif

static void save_frame(int b)
{
	char path[512];
	snprintf(path, sizeof(path), "%s.yuv", cap.out);
	FILE *f = fopen(path, "wb");
	FILE *t;
	snprintf(path, sizeof(path), "%s.txt", cap.out);
	t = fopen(path, "w");
	if (!f || !t) {
		perror("s24-cam: output");
		return;
	}
	fprintf(t, "width %d\nheight %d\nformat 0x%x\nstride %d\nfds %d\n", cap.width, cap.height,
	        cap.format, cap.stride, cap.buf[b].nfds);
	for (int i = 0; i < cap.buf[b].nfds; i++) {
		int fd = cap.buf[b].fds[i];
		off_t size = lseek(fd, 0, SEEK_END);
		void *p = size > 0 ? mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
		fprintf(t, "fd%d_size %lld%s\n", i, (long long)size, p == MAP_FAILED ? " (mmap failed)" : "");
		if (p != MAP_FAILED) {
			fwrite(p, 1, size, f);
			munmap(p, size);
		}
	}
	fprintf(t, "ints");
	for (int i = 0; i < cap.buf[b].nints; i++)
		fprintf(t, " %d", cap.buf[b].ints[i]);
	fprintf(t, "\n");
	fclose(f);
	fclose(t);
	fprintf(stderr, "saved frame to %s.yuv (+ .txt)\n", cap.out);
	cap.saved = TRUE;
}

/* Rotation/flip as an affine element copy: out[oy*oW+ox] = in[base + dy*oy + dx*ox].
 * Done in 64x64 output tiles so the (often column-wise) input reads stay in L1. */
#define DEF_XFORM(NAME, T)                                                                      \
static void NAME(T *out, int oW, int oH, const T *in, long base, long dx, long dy)             \
{                                                                                               \
	const int TB = 64;                                                                      \
	for (int y0 = 0; y0 < oH; y0 += TB) {                                                   \
		int y1 = y0 + TB < oH ? y0 + TB : oH;                                           \
		for (int x0 = 0; x0 < oW; x0 += TB) {                                           \
			int x1 = x0 + TB < oW ? x0 + TB : oW;                                   \
			for (int oy = y0; oy < y1; oy++) {                                      \
				T *o = out + (long)oy * oW + x0;                                \
				const T *s = in + base + dy * oy + dx * x0;                     \
				for (int ox = x0; ox < x1; ox++, s += dx)                       \
					*o++ = *s;                                              \
			}                                                                       \
		}                                                                               \
	}                                                                                       \
}
DEF_XFORM(xform8, uint8_t)
DEF_XFORM(xform16, uint16_t)

/* (rot clockwise, then optional mirror) for an input plane W x H elements, row stride IS
 * elements, output width oW -> input offset = base + dx*ox + dy*oy */
static void xform_coef(int rot, int hflip, long W, long H, long oW, long IS,
                       long *base, long *dx, long *dy)
{
	long A0 = hflip ? oW - 1 : 0, A1 = hflip ? -1 : 1;
	long cx0, cxx, cxy, cy0, cyx, cyy;
	switch (rot) {
	case 90:  cx0 = 0;          cxx = 0;   cxy = 1;  cy0 = H - 1 - A0; cyx = -A1; cyy = 0;  break;
	case 180: cx0 = W - 1 - A0; cxx = -A1; cxy = 0;  cy0 = H - 1;      cyx = 0;   cyy = -1; break;
	case 270: cx0 = W - 1;      cxx = 0;   cxy = -1; cy0 = A0;         cyx = A1;  cyy = 0;  break;
	default:  cx0 = A0;         cxx = A1;  cxy = 0;  cy0 = 0;          cyx = 0;   cyy = 1;  break;
	}
	*base = cy0 * IS + cx0;
	*dx = cyx * IS + cxx;
	*dy = cyy * IS + cxy;
}

static int write_all(int fd, const void *buf, size_t n)
{
	const char *q = buf;
	while (n) {
		ssize_t w = write(fd, q, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		q += w;
		n -= (size_t)w;
	}
	return 0;
}

/* a camera's output size: for a 90/270 rotation the dimensions swap (W,H -> H,W) */
static int cam_w(const Cam *c) { return c->rot == 90 || c->rot == 270 ? cap.height : cap.width; }
static int cam_h(const Cam *c) { return c->rot == 90 || c->rot == 270 ? cap.width : cap.height; }

/* latency probe (env S24CAM_STAMP): the hand-off time, CLOCK_MONOTONIC in microseconds, in the
 * frame's first 8 luma bytes, for a consumer (pwlat) to compare with its own clock */
static void frame_stamp(unsigned char *dst)
{
	static int on = -1;
	if (on < 0)
		on = getenv("S24CAM_STAMP") != NULL;
	if (on) {
		guint64 us = (guint64)(clock_ms(CLOCK_MONOTONIC) * 1000.0);
		memcpy(dst, &us, sizeof(us));
	}
}

/* HAL buffer b -> dst as tightly packed NV12 (cam_w x cam_h of the camera in use), rotated and
 * mirrored as that camera needs. 0 on success. */
static int frame_xform(int b, unsigned char *dst)
{
	int rot = cur->rot, hflip = cur->hflip;
	if (cap.buf[b].nfds < 1)
		return -1;
	int fd = cap.buf[b].fds[0];
	off_t size = lseek(fd, 0, SEEK_END);
	int W = cap.width, H = cap.height, oW = cam_w(cur), oH = cam_h(cur);
	int stride = cap.stride > 0 ? cap.stride : cap.width;
	size_t uvoff = (size_t)stride * H;
	if (size <= 0 || (size_t)size < uvoff + (size_t)stride * (H / 2))
		return -1;
	unsigned char *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return -1;
	long base, dx, dy;
	xform_coef(rot, hflip, W, H, oW, stride, &base, &dx, &dy);
	xform8(dst, oW, oH, p, base, dx, dy);
	xform_coef(rot, hflip, W / 2, H / 2, oW / 2, stride / 2, &base, &dx, &dy);
	xform16((uint16_t *)(dst + (size_t)oW * oH), oW / 2, oH / 2,
	        (const uint16_t *)(p + uvoff), base, dx, dy);
	munmap(p, size);
	return 0;
}

#ifdef S24CAM_PIPEWIRE
/* ---------- PipeWire output (serve mode) -------------------------------------------------- */
/* s24-cam itself is the camera node: a Video/Source stream that DRIVES its graph, one cycle per
 * camera frame (pw_stream_trigger_process), so the app always gets the newest frame with no
 * queue in between; when the app still holds every buffer the frame is dropped, never queued.
 * The camera only runs while an app is streaming. PipeWire's loop runs inside the GLib main
 * loop (its epoll fd), so everything stays on this one thread. */
static struct pw_loop *pwl;
static struct pw_context *pwctx;
static struct pw_core *pwcore;
static struct spa_hook pwcore_hook;

static gboolean pw_dispatch(gint fd, GIOCondition cond, gpointer user)
{
	pw_loop_iterate(pwl, 0);
	return G_SOURCE_CONTINUE;
}

static void pw_on_state(void *d, enum pw_stream_state old, enum pw_stream_state st, const char *err)
{
	Cam *c = d;
	int was = c->active;
	c->active = st == PW_STREAM_STATE_STREAMING;
	fprintf(stderr, "[pw] %s: %s -> %s%s%s\n", c->desc, pw_stream_state_as_string(old),
	        pw_stream_state_as_string(st), err ? ": " : "", err ? err : "");
	if (c->active && !was)
		cam_wanted(c);                          /* an app opened this camera: run it */
	else if (!c->active && was && c == cur)
		cam_idle_soon();                        /* it left: close the camera after a while */
	if (st == PW_STREAM_STATE_ERROR)
		quit_when_drained();
}

static void pw_on_param(void *d, uint32_t id, const struct spa_pod *param)
{
	Cam *c = d;
	if (!param || id != SPA_PARAM_Format)
		return;
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *p[2];
	p[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(cam_w(c) * cam_h(c) * 3 / 2),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(cam_w(c)),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
	/* a header per buffer: its pts is what the app's video sink syncs to. Without it GStreamer's
	 * pipewiresrc stamps every frame with the same time, the sink finds each one "late" and drops
	 * it, and the picture freezes after the first frames */
	p[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->pws, p, 2);
	fprintf(stderr, "[pw] %s: format set, %dx%d NV12\n", c->desc, cam_w(c), cam_h(c));
}

static void pw_on_core_error(void *d, uint32_t id, int seq, int res, const char *msg)
{
	fprintf(stderr, "[pw] core error %u: %s (%d)\n", id, msg ? msg : "", res);
	if (id == PW_ID_CORE && res == -EPIPE)     /* PipeWire went away */
		quit_when_drained();
}

static const struct pw_stream_events pws_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pw_on_state,
	.param_changed = pw_on_param,
};
static const struct pw_core_events pwcore_events = {
	PW_VERSION_CORE_EVENTS,
	.error = pw_on_core_error,
};

/* one Video/Source node per camera. api.libcamera.location tells apps which way it faces:
 * GNOME Snapshot mirrors the PREVIEW of every camera not marked "back" (photos and videos are
 * saved unmirrored), so frames go out upright and unmirrored, and the mark does the rest. */
static int pw_setup(void)
{
	pw_init(NULL, NULL);
	pwl = pw_loop_new(NULL);
	if (!pwl)
		return -1;
	pw_loop_enter(pwl);
	pwctx = pw_context_new(pwl, NULL, 0);
	pwcore = pwctx ? pw_context_connect(pwctx, NULL, 0) : NULL;
	if (!pwcore) {
		fprintf(stderr, "[pw] cannot connect to PipeWire (XDG_RUNTIME_DIR=%s): %s\n",
		        getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "unset", strerror(errno));
		return -1;
	}
	pw_core_add_listener(pwcore, &pwcore_hook, &pwcore_events, NULL);
	/* GStreamer's device list puts the newest node first and Snapshot opens the first camera
	 * when it remembers none: create them last-to-first so the first ID (Back) leads */
	for (int i = ncams - 1; i >= 0; i--) {
		Cam *c = &cams[i];
		static const char *loc[] = { "front", "back", "external" };
		c->pws = pw_stream_new(pwcore, c->node, pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_ROLE, "Camera",
			PW_KEY_NODE_NAME, c->node,
			PW_KEY_NODE_DESCRIPTION, c->desc,
			"api.libcamera.location", c->facing >= 0 && c->facing < 3 ? loc[c->facing] : "external",
			NULL));
		if (!c->pws)
			return -1;
		pw_stream_add_listener(c->pws, &c->hook, &pws_events, c);
		uint8_t buf[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		const struct spa_pod *p[1];
		p[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(cam_w(c), cam_h(c))),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(30, 1)));
		if (pw_stream_connect(c->pws, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		                      PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, p, 1) < 0) {
			fprintf(stderr, "[pw] %s: stream connect failed\n", c->desc);
			return -1;
		}
		fprintf(stderr, "[pw] node %s (%s, %s) %dx%d NV12 30 fps, rotate %d%s\n", c->node, c->desc,
		        c->facing >= 0 && c->facing < 3 ? loc[c->facing] : "?", cam_w(c), cam_h(c), c->rot,
		        c->hflip ? " + mirror" : "");
	}
	g_unix_fd_add(pw_loop_get_fd(pwl), G_IO_IN, pw_dispatch, NULL);
	fprintf(stderr, "[pw] a camera runs only while an app is watching it\n");
	return 0;
}

/* newest frame straight into a free PipeWire buffer of the camera in use, then one graph cycle;
 * 1 = delivered */
static int pw_frame(int b)
{
	struct pw_buffer *pb = cur->active ? pw_stream_dequeue_buffer(cur->pws) : NULL;
	if (!pb)
		return 0;                               /* the app holds every buffer: drop */
	struct spa_data *d = &pb->buffer->datas[0];
	size_t need = (size_t)cam_w(cur) * cam_h(cur) * 3 / 2;
	int ok = d->data && d->maxsize >= need && frame_xform(b, d->data) == 0;
	if (ok)
		frame_stamp(d->data);
	struct spa_meta_header *h = spa_buffer_find_meta_data(pb->buffer, SPA_META_Header, sizeof(*h));
	if (h) {                /* hand-off time on CLOCK_MONOTONIC, PipeWire's (and GStreamer's) clock */
		static uint64_t seq;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		h->flags = ok ? 0 : SPA_META_HEADER_FLAG_CORRUPTED;
		h->offset = 0;
		h->pts = SPA_TIMESPEC_TO_NSEC(&ts);
		h->dts_offset = 0;
		h->seq = seq++;
	}
	d->chunk->offset = 0;
	d->chunk->size = ok ? need : 0;
	d->chunk->stride = cam_w(cur);
	d->chunk->flags = ok ? 0 : SPA_CHUNK_FLAG_CORRUPTED;
	pw_stream_queue_buffer(cur->pws, pb);
	if (ok)
		pw_stream_trigger_process(cur->pws);
	return ok;
}
#endif

/* stream/serve mode: one finished camera frame -> the app (pipe or PipeWire), plus stats */
static void stream_frame(int b)
{
	double t0 = now_ms();
	int sent = 0;
	if (!st_t0) {
		st_t0 = t0;
		if (!pw_mode)           /* room for a whole frame: one write per frame, fewer wakeups */
			fcntl(1, F_SETPIPE_SZ, 4 << 20);
	}
#ifdef S24CAM_PIPEWIRE
	if (pw_mode)
		sent = pw_frame(b);
	else
#endif
	{
		static unsigned char *out;
		static size_t outcap;
		size_t need = (size_t)cam_w(cur) * cam_h(cur) * 3 / 2;
		if (outcap < need) {
			free(out);
			out = malloc(need);
			outcap = out ? need : 0;
		}
		if (out && frame_xform(b, out) == 0) {
			frame_stamp(out);
			sent = write_all(1, out, need) == 0;
		}
	}
	double t1 = now_ms();
	if (sent)
		st_proc_sum += t1 - t0;
	else
		st_drop++;
	static int every;               /* stats line every second with S24CAM_STATS, else every 30 s */
	if (!every)
		every = getenv("S24CAM_STATS") ? 30 : 900;
	/* S24CAM_FACETEST=N (test): face priority off/on every N frames within the session */
	static int facetest = -1;
	if (facetest < 0)
		facetest = getenv("S24CAM_FACETEST") ? atoi(getenv("S24CAM_FACETEST")) : 0;
	if (facetest > 0 && cur->facing == 0 && (st_n + 1) % facetest == 0) {
		frame_color(b, g_face ? "phase face-priority" : "phase plain");
		g_face = !g_face;
		g_free(cap.settings);
		cap.settings = default_request(&cap.settings_len);
		cap.resend = TRUE;
		fprintf(stderr, "[face] frame %d: face priority %s from the next request\n", st_n + 1, g_face ? "ON" : "off");
	}
	if (++st_n % every == 0) {
		if (getenv("S24CAM_COLOR"))
			frame_color(b, cur->desc);
		int got = every - st_drop;
		fprintf(stderr, "[stream] %.1f fps, %d dropped | HAL request->frame %.0f ms | exposure->frame %.0f ms"
		        " | copy %.1f ms | depth %d | AF %s, lens %s (%.2f dpt) | AE %d, %.1f ms ISO %d\n",
		        every * 1000.0 / (t1 - st_t0), st_drop, st_lat_sum / every, st_sens_n ? st_sens_sum / st_sens_n : -1.0,
		        got > 0 ? st_proc_sum / got : 0.0, g_depth, af_state_name(g_af_state),
		        g_lens_state == 1 ? "moving" : g_lens_state == 0 ? "still" : "?", g_focus,
		        g_ae_state, g_exposure / 1e6, g_iso);
		g_meta_inline = g_meta_fmq = 0;
		st_t0 = t1;
		st_lat_sum = st_proc_sum = st_sens_sum = 0;
		st_sens_n = st_drop = 0;
	}
}

static void wait_fence(Handle *h)
{
	for (int i = 0; i < h->nfds; i++) {
		struct pollfd p = { h->fds[i], POLLIN, 0 };
		poll(&p, 1, 1000);
	}
	close_handle(h);
}

static int buffer_index(gint64 id)
{
	return id >= 1 && id <= cap.nbuf ? (int)id - 1 : -1;
}

/* one StreamBuffer from a result: the buffer handle (unused here), fences */
static void read_stream_buffer(GBinderReader *r, gint32 *stream, gint64 *bufid, gint32 *status)
{
	GBinderReader p;
	gboolean nn;
	Handle h, acq, rel;
	*stream = -1;
	*bufid = 0;
	*status = 1;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return;
	gbinder_reader_read_int32(&p, stream);
	gbinder_reader_read_int64(&p, bufid);
	read_handle(&p, &h);
	close_handle(&h);
	gbinder_reader_read_int32(&p, status);
	read_handle(&p, &acq);
	close_handle(&acq);
	read_handle(&p, &rel);
	wait_fence(&rel);
	gbinder_reader_finish_parcelable(&p);
}

static void result_meta(const guint8 *m, gsize len, int frame)
{
	uint32_t c;
	const void *v;
	if ((v = meta_find(m, len, TAG_SS_AF_POS_CUR, &c, NULL)) && c)
		memcpy(&g_af_pos, v, sizeof(g_af_pos));
	if ((v = meta_find(m, len, TAG_CONTROL_AF_STATE, &c, NULL)) && c) {
		int s = *(const guint8 *)v;
		if (s != g_af_state && getenv("S24CAM_STATS"))  /* focus state changes */
			fprintf(stderr, "[af] frame %d: %s -> %s (lens position %d)\n", frame,
			        af_state_name(g_af_state), af_state_name(s), g_af_pos);
		g_af_state = s;
	}
	if ((v = meta_find(m, len, TAG_CONTROL_AE_STATE, &c, NULL)) && c)
		g_ae_state = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_LENS_STATE, &c, NULL)) && c)
		g_lens_state = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_LENS_FOCUS_DISTANCE, &c, NULL)) && c)
		memcpy(&g_focus, v, sizeof(g_focus));
	if ((v = meta_find(m, len, TAG_SENSOR_EXPOSURE_TIME, &c, NULL)) && c)
		memcpy(&g_exposure, v, sizeof(g_exposure));
	if ((v = meta_find(m, len, TAG_SENSOR_SENSITIVITY, &c, NULL)) && c)
		memcpy(&g_iso, v, sizeof(g_iso));
	if ((v = meta_find(m, len, TAG_CONTROL_AWB_MODE, &c, NULL)) && c)
		g_awb_mode = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_CONTROL_AWB_STATE, &c, NULL)) && c)
		g_awb_state = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_COLOR_CORRECTION_MODE, &c, NULL)) && c)
		g_cc_mode = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_COLOR_CORRECTION_GAINS, &c, NULL)) && c >= 4)
		memcpy(g_wb, v, sizeof(g_wb));
	if ((v = meta_find(m, len, TAG_CONTROL_MODE, &c, NULL)) && c)
		g_ctl_mode = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_CONTROL_SCENE_MODE, &c, NULL)) && c)
		g_scene = *(const guint8 *)v;
	if ((v = meta_find(m, len, TAG_STATISTICS_FACE_DETECT_MODE, &c, NULL)) && c)
		g_fd_mode = *(const guint8 *)v;
	if (meta_find(m, len, TAG_STATISTICS_FACE_SCORES, &c, NULL) || meta_find(m, len, TAG_STATISTICS_FACE_DETECT_MODE, &c, NULL))
		g_faces = meta_find(m, len, TAG_STATISTICS_FACE_SCORES, &c, NULL) ? (int)c : 0;
	if ((v = meta_find(m, len, TAG_COLOR_CORRECTION_TRANSFORM, &c, NULL)) && c >= 9)
		for (int i = 0; i < 9; i++) {
			const gint32 *r = (const gint32 *)v + 2 * i;
			g_ccm[i] = r[1] ? (float)r[0] / r[1] : 0;
		}
}

/* colour of HAL buffer b (NV12, cap.width x cap.height): mean Y, Cb, Cr and the mean R, G, B
 * they give (BT.601 full range), whole frame and centre quarter, plus the HAL's white balance */
static void frame_color(int b, const char *what)
{
	if (cap.buf[b].nfds < 1)
		return;
	int fd = cap.buf[b].fds[0];
	off_t size = lseek(fd, 0, SEEK_END);
	int W = cap.width, H = cap.height, S = cap.stride > 0 ? cap.stride : cap.width;
	if (size <= 0 || (size_t)size < (size_t)S * H * 3 / 2)
		return;
	const guint8 *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return;
	for (int part = 0; part < 2; part++) {
		int x0 = part ? W / 4 : 0, x1 = part ? 3 * W / 4 : W, y0 = part ? H / 4 : 0, y1 = part ? 3 * H / 4 : H;
		double sy = 0, su = 0, sv = 0, sr = 0, sg = 0, sb = 0;
		long n = 0;
		for (int y = y0 & ~1; y < y1; y += 8)
			for (int x = x0 & ~1; x < x1; x += 8) {
				double Y = p[(size_t)y * S + x];
				const guint8 *uv = p + (size_t)S * H + (size_t)(y / 2) * S + (x & ~1);
				double U = uv[0] - 128.0, V = uv[1] - 128.0;
				sy += Y; su += U; sv += V;
				sr += Y + 1.402 * V;
				sg += Y - 0.344136 * U - 0.714136 * V;
				sb += Y + 1.772 * U;
				n++;
			}
		if (n)
			fprintf(stderr, "[color] %s %s: Y %.1f Cb %+.1f Cr %+.1f | RGB %.1f %.1f %.1f (B/G %.3f R/G %.3f)\n",
			        what, part ? "centre" : "frame", sy / n, su / n, sv / n, sr / n, sg / n, sb / n,
			        sg ? sb / sg : 0, sg ? sr / sg : 0);
	}
	fprintf(stderr, "[color] %s HAL: AWB mode %d state %d | colour correction mode %d, gains R %.3f G %.3f %.3f B %.3f"
	        " | matrix %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f | AE %d %.1f ms ISO %d"
	        " | control mode %d scene %d face detect %d, faces %d\n", what,
	        g_awb_mode, g_awb_state, g_cc_mode, g_wb[0], g_wb[1], g_wb[2], g_wb[3], g_ccm[0], g_ccm[1],
	        g_ccm[2], g_ccm[3], g_ccm[4], g_ccm[5], g_ccm[6], g_ccm[7], g_ccm[8], g_ae_state,
	        g_exposure / 1e6, g_iso, g_ctl_mode, g_scene, g_fd_mode, g_faces);
	munmap((void *)p, size);
}

static void on_capture_results(GBinderReader *r)
{
	gint32 n = 0;
	gbinder_reader_read_int32(r, &n);
	if (getenv("S24CAM_CBDEBUG")) fprintf(stderr, "[cb] processCaptureResult n=%d\n", n);
	for (int i = 0; i < n; i++) {
		GBinderReader p;
		gboolean nn;
		if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
			continue;
		gint32 frame = 0, nout = 0;
		gint64 fmq = 0;
		gsize mlen = 0;
		gbinder_reader_read_int32(&p, &frame);
		gbinder_reader_read_int64(&p, &fmq);
		guint8 *md = read_metadata(&p, &mlen);
		if (md && mlen >= sizeof(MetaHeader)) {
			result_meta(md, mlen, frame);
			g_meta_inline++;
		}
		if (fmq > 0)            /* metadata went to the result FMQ, which we don't read */
			g_meta_fmq++;
		g_free(md);
		gbinder_reader_read_int32(&p, &nout);
		if (getenv("S24CAM_CBDEBUG")) fprintf(stderr, "[cb]  frame=%d metalen=%zu nout=%d\n", frame, mlen, nout);
		for (int k = 0; k < nout; k++) {
			gint32 stream, status;
			gint64 bufid;
			read_stream_buffer(&p, &stream, &bufid, &status);
			int b = buffer_index(bufid);
			if (b < 0)                 /* Samsung HAL returns bufferId 0: use the buffer we sent for this frame */
				b = (frame >= 0 && cap.fbuf[frame % 64] >= 0) ? cap.fbuf[frame % 64] : -1;
			if (getenv("S24CAM_CBDEBUG")) fprintf(stderr, "[cb]   out[%d] stream=%d bufid=%lld status=%d -> idx=%d\n", k, stream, (long long)bufid, status, b);
			if (b < 0)
				continue;
			cap.busy[b] = FALSE;
			cap.done_frames++;
			last_progress = now_ms();
			if (status)
				fprintf(stderr, "frame %d: buffer %d error\n", frame, b);
			if (stream_mode) {
				st_lat_sum += now_ms() - st_submit[frame & 63];
				if (st_shutter[frame & 63] > 0) {   /* HAL timestamps are CLOCK_BOOTTIME */
					double l = clock_ms(CLOCK_BOOTTIME) - st_shutter[frame & 63] / 1e6;
					if (l > 0 && l < 5000) {
						st_sens_sum += l;
						st_sens_n++;
					}
					st_shutter[frame & 63] = 0;
				}
				if (!status)
					stream_frame(b);
			} else {
				if (cap.done_frames == 1 || cap.done_frames % 10 == 0)
					fprintf(stderr, "frame %d done (buffer %d, status %d)\n", frame, b, status);
				if (!status && cap.done_frames >= cap.frames && !cap.saved) {
					frame_color(b, cur->desc);
					save_frame(b);
				}
			}
		}
		gbinder_reader_finish_parcelable(&p);
	}
	if (cap.saved)
		quit_when_drained();
	else
		g_idle_add(submit_next, NULL);
}

static void on_notify(GBinderReader *r)
{
	gint32 n = 0;
	gbinder_reader_read_int32(r, &n);
	for (int i = 0; i < n; i++) {
		GBinderReader p;
		gboolean nn;
		gint32 nonnull = 0, tag = 0, frame = 0, a = 0, b = 0;
		gbinder_reader_read_int32(r, &nonnull);   /* union element: non-null, tag, value */
		gbinder_reader_read_int32(r, &tag);
		if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
			return;
		gbinder_reader_read_int32(&p, &frame);
		if (tag == 0) {           /* ErrorMsg: frameNumber, errorStreamId, errorCode */
			gbinder_reader_read_int32(&p, &a);
			gbinder_reader_read_int32(&p, &b);
			fprintf(stderr, "notify: error %d on frame %d stream %d\n", b, frame, a);
			cap.errors++;
		} else if (tag == 1) {    /* ShutterMsg: frameNumber, timestamp (start of exposure), readout */
			gint64 ts = 0;
			gbinder_reader_read_int64(&p, &ts);
			st_shutter[frame & 63] = ts;
			static int shown;
			if (!shown++)
				fprintf(stderr, "[ts] frame %d shutter %.1f ms; now boottime %.1f monotonic %.1f\n", frame,
				        ts / 1e6, clock_ms(CLOCK_BOOTTIME), clock_ms(CLOCK_MONOTONIC));
		}
		gbinder_reader_finish_parcelable(&p);
	}
	if (cap.errors > 20) {
		fprintf(stderr, "too many errors\n");
		quit_when_drained();
	}
}

static GBinderLocalReply *cb_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                      guint code, guint flags, int *status, void *user)
{
	GBinderLocalReply *rep = gbinder_local_object_new_reply(obj);
	GBinderWriter w;
	GBinderReader r;
	gbinder_local_reply_init_writer(rep, &w);
	*status = 0;
	last_cb_ms = now_ms();          /* see settled(): no close while the HAL is still talking */
	if (code == AIDL_GET_VERSION) {
		gbinder_writer_append_int32(&w, 0);
		gbinder_writer_append_int32(&w, 2);
		return rep;
	}
	if (code == AIDL_GET_HASH) {
		gbinder_writer_append_int32(&w, 0);
		gbinder_writer_append_string16(&w, DEVICE_HASH);
		return rep;
	}
	gbinder_remote_request_init_reader(req, &r);
	if (getenv("S24CAM_CBDEBUG")) fprintf(stderr, "[cb] transact code=%u\n", code);
	switch (code) {
	case CB_NOTIFY:
		on_notify(&r);
		break;
	case CB_PROCESS_CAPTURE_RESULT:
		on_capture_results(&r);
		break;
	case CB_REQUEST_STREAM_BUFFERS:
		fprintf(stderr, "HAL asked for buffers (HAL buffer management): not supported yet\n");
		break;
	case CB_RETURN_STREAM_BUFFERS:
		fprintf(stderr, "HAL returned buffers\n");
		break;
	}
	gbinder_writer_append_int32(&w, 0);     /* EX_NONE (void methods) */
	return rep;
}

static void write_stream(GBinderWriter *w)
{
	GBinderWriter s;
	gbinder_writer_start_parcelable(w, &s);
	gbinder_writer_append_int32(&s, 0);                 /* id */
	gbinder_writer_append_int32(&s, 0);                 /* streamType OUTPUT */
	gbinder_writer_append_int32(&s, cap.width);
	gbinder_writer_append_int32(&s, cap.height);
	gbinder_writer_append_int32(&s, PIXEL_FORMAT_YCBCR_420_888);
	gbinder_writer_append_int64(&s, USAGE_CPU_READ_OFTEN);  /* consumer usage */
	gbinder_writer_append_int32(&s, 0x08C20000);        /* dataSpace JFIF (BT.601 full range) */
	gbinder_writer_append_int32(&s, 0);                 /* rotation 0 */
	gbinder_writer_append_string16(&s, "");             /* physicalCameraId */
	gbinder_writer_append_int32(&s, 0);                 /* bufferSize */
	gbinder_writer_append_int32(&s, -1);                /* groupId */
	gbinder_writer_append_int32(&s, 1);                 /* sensorPixelModesUsed: [DEFAULT] */
	gbinder_writer_append_int32(&s, 0);
	gbinder_writer_append_int64(&s, 1);                 /* dynamicRangeProfile STANDARD */
	gbinder_writer_append_int64(&s, 0);                 /* useCase DEFAULT */
	gbinder_writer_append_int32(&s, -1);                /* colorSpace UNSPECIFIED */
	gbinder_writer_finish_parcelable(&s);
}

static int configure(void)
{
	GBinderLocalRequest *req = gbinder_client_new_request(cap.session);
	GBinderWriter w, c;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_start_parcelable(&w, &c);            /* StreamConfiguration */
	gbinder_writer_append_int32(&c, 1);                 /* streams[1] */
	write_stream(&c);
	gbinder_writer_append_int32(&c, 0);                 /* operationMode NORMAL */
	write_metadata(&c, NULL, 0);                        /* sessionParams: empty */
	gbinder_writer_append_int32(&c, 0);                 /* streamConfigCounter */
	gbinder_writer_append_bool(&c, FALSE);              /* multiResolutionInputImage */
	gbinder_writer_append_int64(&c, 0);                 /* logId */
	gbinder_writer_finish_parcelable(&c);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(cap.session, SES_CONFIGURE_STREAMS, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	int maxbuf = -1;
	if (reply_ok(rep, st, "configureStreams", &r)) {
		gint32 n = 0;
		gbinder_reader_read_int32(&r, &n);
		for (int i = 0; i < n; i++) {
			GBinderReader p;
			gboolean nn;
			gint32 id = 0, fmt = 0, mb = 0, ds = 0;
			gint64 prod = 0, cons = 0;
			if (!gbinder_reader_start_parcelable(&r, &p, &nn) || !nn)
				continue;
			gbinder_reader_read_int32(&p, &id);
			gbinder_reader_read_int32(&p, &fmt);
			gbinder_reader_read_int64(&p, &prod);
			gbinder_reader_read_int64(&p, &cons);
			gbinder_reader_read_int32(&p, &mb);
			gbinder_reader_read_int32(&p, &ds);
			gbinder_reader_finish_parcelable(&p);
			fprintf(stderr, "stream %d: format 0x%x producer usage 0x%llx consumer 0x%llx max buffers %d dataspace 0x%x\n",
			       id, fmt, (unsigned long long)prod, (unsigned long long)cons, mb, ds);
			cap.usage = USAGE_CPU_READ_OFTEN | USAGE_CAMERA_OUTPUT | (guint64)prod;
			if (fmt && fmt != PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
				cap.format = fmt;
			maxbuf = mb;
		}
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	return maxbuf;
}

static int allocate(int count)
{
	/* Samsung's allocator is IAllocator V1: allocate(byte[] descriptor, count). Its descriptor is
	 * their own BufferDescriptor struct (from the mapper's create_descriptor), 296 bytes:
	 *   u64 length_name; char name[256]; u32 width,height,layerCount; i32 format;
	 *   u64 usage; u64 reservedSize   (the fixed 256-byte name is the difference from gralloc4) */
	static const char name[] = "s24-cam";
	guint8 desc[296];
	memset(desc, 0, sizeof(desc));
	guint64 nl = strlen(name);
	guint32 u32;
	gint32 i32;
	guint64 u64;
	memcpy(desc + 0, &nl, 8);
	memcpy(desc + 8, name, nl);
	u32 = cap.width; memcpy(desc + 264, &u32, 4);
	u32 = cap.height; memcpy(desc + 268, &u32, 4);
	u32 = 1; memcpy(desc + 272, &u32, 4);                   /* layerCount */
	i32 = cap.format; memcpy(desc + 276, &i32, 4);
	u64 = cap.usage; memcpy(desc + 280, &u64, 8);
	u64 = 0; memcpy(desc + 288, &u64, 8);                   /* reservedSize */
	gsize n = sizeof(desc);
	GBinderLocalRequest *req = gbinder_client_new_request(cap.alloc);
	GBinderWriter w;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_byte_array(&w, desc, n);
	gbinder_writer_append_int32(&w, count);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(cap.alloc, ALLOC_ALLOCATE, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	int got = 0;
	if (reply_ok(rep, st, "allocate", &r)) {
		GBinderReader p;
		gboolean nn;
		if (gbinder_reader_start_parcelable(&r, &p, &nn) && nn) {
			gint32 stride = 0, n = 0;
			gbinder_reader_read_int32(&p, &stride);
			gbinder_reader_read_int32(&p, &n);
			for (int i = 0; i < n && got < MAXBUF; i++)
				if (read_handle(&p, &cap.buf[got]))
					got++;
			gbinder_reader_finish_parcelable(&p);
			cap.stride = stride;
			fprintf(stderr, "allocated %d buffer(s), stride %d, %d fd(s) and %d int(s) each\n", got, stride,
			       got ? cap.buf[0].nfds : 0, got ? cap.buf[0].nints : 0);
		}
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	return got;
}

static gboolean submit_next(gpointer unused)
{
	int b = -1, inflight = 0;
	if (!cap.session || quitting)
		return G_SOURCE_REMOVE;
	for (int i = 0; i < cap.nbuf; i++)
		if (!cap.busy[i]) {
			if (b < 0) b = i;
		} else
			inflight++;
	if (b < 0 || cap.next_frame >= cap.frames + 8)
		return G_SOURCE_REMOVE;
	/* stream mode: cap frames-in-flight low to keep latency down (default 2); re-armed per result */
	if (stream_mode) {
		if (g_depth < 0) { const char *d = getenv("S24CAM_DEPTH"); g_depth = d ? atoi(d) : 4; }
		if (inflight >= g_depth)
			return G_SOURCE_REMOVE;
		if (pw_mode && (want || !cur->active || screen_off))  /* switching, nobody watching, dark */
			return G_SOURCE_REMOVE;
	}
	GBinderLocalRequest *req = gbinder_client_new_request(cap.session);
	GBinderWriter w, q, s;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_int32(&w, 1);                 /* requests[1] */
	gbinder_writer_start_parcelable(&w, &q);            /* CaptureRequest */
	gbinder_writer_append_int32(&q, cap.next_frame);
	gbinder_writer_append_int64(&q, 0);                 /* fmqSettingsSize: inline */
	if (cap.next_frame == 0 || cap.resend) {            /* settings once; empty = same again */
		write_metadata(&q, cap.settings, cap.settings_len);
		cap.resend = FALSE;
	} else {
		write_metadata(&q, NULL, 0);
	}
	gbinder_writer_start_parcelable(&q, &s);            /* inputBuffer: none */
	gbinder_writer_append_int32(&s, -1);
	gbinder_writer_append_int64(&s, 0);
	write_handle(&s, NULL);
	gbinder_writer_append_int32(&s, 0);
	write_handle(&s, NULL);
	write_handle(&s, NULL);
	gbinder_writer_finish_parcelable(&s);
	gbinder_writer_append_int32(&q, 0);                 /* inputWidth */
	gbinder_writer_append_int32(&q, 0);                 /* inputHeight */
	gbinder_writer_append_int32(&q, 1);                 /* outputBuffers[1] */
	gbinder_writer_start_parcelable(&q, &s);
	gbinder_writer_append_int32(&s, 0);                 /* streamId */
	gbinder_writer_append_int64(&s, b + 1);             /* bufferId */
	write_handle(&s, cap.sent_handle[b] ? NULL : &cap.buf[b]);  /* handle once, then cached */
	gbinder_writer_append_int32(&s, 0);                 /* status OK */
	write_handle(&s, NULL);                             /* acquireFence: none */
	write_handle(&s, NULL);                             /* releaseFence */
	gbinder_writer_finish_parcelable(&s);
	gbinder_writer_append_int32(&q, 0);                 /* physicalCameraSettings[] */
	gbinder_writer_finish_parcelable(&q);
	gbinder_writer_append_int32(&w, 0);                 /* cachesToRemove[] */
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(cap.session, SES_PROCESS_CAPTURE_REQUEST, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	if (reply_ok(rep, st, "processCaptureRequest", &r)) {
		if (!inflight)          /* the watchdog's clock starts with the first frame asked for */
			last_progress = now_ms();
		cap.busy[b] = TRUE;
		cap.sent_handle[b] = TRUE;
		cap.fbuf[cap.next_frame % 64] = b;
		st_submit[cap.next_frame % 64] = now_ms();
		cap.next_frame++;
	} else {
		g_main_loop_quit(loop);
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	return G_SOURCE_CONTINUE;   /* keep the pipeline filled while buffers are free */
}

static gboolean on_quit_signal(gpointer unused)
{
	fprintf(stderr, "signal: closing the camera once its frames are back\n");
	quit_when_drained();
	return G_SOURCE_REMOVE;
}

static gboolean on_timeout(gpointer unused)
{
	fprintf(stderr, "timeout: %d frame(s) done\n", cap.done_frames);
	quit_when_drained();
	return G_SOURCE_REMOVE;
}

/* open the camera device and configure the preview stream (buffers are allocated once and
 * reused by every session). A clean close + open needs no cameraserver re-prime. 0 = ok */
static void cam_close(void);
static int cam_open(void)
{
	double t0 = now_ms();
	GBinderLocalRequest *req = gbinder_client_new_request(cur->dev);
	GBinderWriter w;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_local_object(&w, cap.cb);
	int st = 0;
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(cur->dev, DEV_OPEN, req, &st);
	gbinder_local_request_unref(req);
	GBinderReader r;
	if (!reply_ok(rep, st, "open", &r))
		return -1;
	GBinderRemoteObject *so = gbinder_reader_read_object(&r);
	gbinder_remote_reply_unref(rep);
	if (!so) {
		fprintf(stderr, "s24-cam: open returned no session\n");
		return -1;
	}
	cap.session = gbinder_client_new(so, SESSION_IFACE);
	gbinder_remote_object_unref(so);
	fprintf(stderr, "camera %s (%s) open\n", cur->id, cur->desc);

	req = gbinder_client_new_request(cap.session);
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_int32(&w, 1);                 /* RequestTemplate PREVIEW */
	rep = gbinder_client_transact_sync_reply(cap.session, SES_CONSTRUCT_DEFAULT_REQUEST_SETTINGS, req, &st);
	gbinder_local_request_unref(req);
	/* S24CAM_TEMPLATE=1: request = this camera's own template + our settings (rebuilt per camera);
	 * "alt" switches between ours and the template on every open (A/B tests) */
	static int template_env = -1, opens;
	if (template_env < 0) {
		const char *t = getenv("S24CAM_TEMPLATE");
		template_env = !t ? 0 : !strcmp(t, "alt") ? 2 : atoi(t) != 0;
	}
	int use_template = template_env == 2 ? opens++ & 1 : template_env;
	if (template_env == 2)
		fprintf(stderr, "A/B: this open uses %s\n", use_template ? "the camera's template" : "our 6 settings");
	/* settings per camera (the front one adds face priority), built at every open */
	g_free(cap.settings);
	cap.settings = NULL;
	cap.settings_len = 0;
	cap.resend = FALSE;
	if (reply_ok(rep, st, "constructDefaultRequestSettings", &r)) {
		gsize hal_len = 0;
		guint8 *hal = read_metadata(&r, &hal_len);
		fprintf(stderr, "HAL default template: %zu bytes, %u entries\n", hal_len,
		       hal_len >= sizeof(MetaHeader) ? ((MetaHeader *)hal)->entry_count : 0);
		if (getenv("S24CAM_DUMPTPL"))
			dump_metadata(cur->desc, hal, hal_len);
		if (use_template && (cap.settings = template_request(hal, hal_len, &cap.settings_len)))
			fprintf(stderr, "request settings: the camera's template + ours, %u entries\n",
			        ((MetaHeader *)cap.settings)->entry_count);
		g_free(hal);
	}
	if (rep)
		gbinder_remote_reply_unref(rep);
	if (!cap.settings) {    /* the HAL's template is a skeleton; build a complete request ourselves */
		cap.settings = default_request(&cap.settings_len);
		fprintf(stderr, "default preview settings: %zu bytes, %u entries\n", cap.settings_len,
		       cap.settings_len >= sizeof(MetaHeader) ? ((MetaHeader *)cap.settings)->entry_count : 0);
		if (getenv("S24CAM_DUMPSET")) {
			fprintf(stderr, "settings hex:");
			for (gsize i = 0; i < cap.settings_len && i < 96; i++)
				fprintf(stderr, " %02x", cap.settings[i]);
			fprintf(stderr, "\n");
		}
	}
	int maxbuf = configure();
	if (maxbuf <= 0) {
		cam_close();
		return -1;
	}
	if (cap.nbuf && cap.usage != cap.buf_usage) {   /* this camera wants other buffer flags */
		for (int i = 0; i < cap.nbuf; i++)
			close_handle(&cap.buf[i]);
		cap.nbuf = 0;
	}
	if (!cap.nbuf && (cap.nbuf = allocate(MIN(maxbuf, MAXBUF))) <= 0) {
		cap.nbuf = 0;
		cam_close();
		return -1;
	}
	cap.buf_usage = cap.usage;
	/* a new session: frame numbers restart at 0 (settings go with frame 0) and the HAL has
	 * seen none of our buffer handles yet */
	cap.next_frame = 0;
	for (int i = 0; i < MAXBUF; i++)
		cap.busy[i] = cap.sent_handle[i] = FALSE;
	for (int i = 0; i < 64; i++)
		cap.fbuf[i] = -1;
	st_t0 = 0;
	st_n = 0;
	wake_hold(1);
	fprintf(stderr, "camera ready in %.0f ms\n", now_ms() - t0);
	return 0;
}

static void cam_close(void)
{
	if (!cap.session)
		return;
	int st = 0;
	GBinderLocalRequest *req = gbinder_client_new_request(cap.session);
	GBinderRemoteReply *rep = gbinder_client_transact_sync_reply(cap.session, SES_CLOSE, req, &st);
	gbinder_local_request_unref(req);
	if (rep)
		gbinder_remote_reply_unref(rep);
	gbinder_client_unref(cap.session);
	cap.session = NULL;
	wake_hold(0);
	fprintf(stderr, "camera closed (%d frames so far, %d errors)\n", cap.done_frames, cap.errors);
}

#ifdef S24CAM_PIPEWIRE
/* serve mode: with no app watching, close the camera (sensor off, the HAL idle) after
 * S24CAM_IDLE_CLOSE seconds (default 5; a quick app restart keeps it open) */
static guint idle_timer, switch_timer;

static gboolean on_idle_close(gpointer unused)
{
	idle_timer = 0;
	if (!cur || (cur->active && !screen_off) || want || !cap.session)
		return G_SOURCE_REMOVE;
	if (!settled()) {               /* let the last requests finish first */
		idle_timer = g_timeout_add(100, on_idle_close, NULL);
		return G_SOURCE_REMOVE;
	}
	fprintf(stderr, "no app watching: closing the camera\n");
	cam_close();
	return G_SOURCE_REMOVE;
}

static void cam_idle_soon(void)
{
	const char *e = getenv("S24CAM_IDLE_CLOSE");
	int s = e ? atoi(e) : 5;
	for (int i = 0; !screen_off && i < ncams; i++) /* another camera still wanted: go there */
		if (cams[i].active && &cams[i] != cur) {
			cam_wanted(&cams[i]);
			return;
		}
	if (idle_timer)
		g_source_remove(idle_timer);
	idle_timer = s >= 0 ? g_timeout_add_seconds(s, on_idle_close, NULL) : 0;
}

/* run `want`: once the current camera's in-flight frames are back, close it, open the other.
 * (Closing with frames still out could deadlock: the HAL delivers them to this thread, which
 * would be waiting inside close().) */
static gboolean try_switch(gpointer unused)
{
	switch_timer = 0;
	if (!want)
		return G_SOURCE_REMOVE;
	if (cap.session && cur != want) {
		if (!settled()) {
			switch_timer = g_timeout_add(10, try_switch, NULL);
			return G_SOURCE_REMOVE;
		}
		double t0 = now_ms();
		cam_close();
		fprintf(stderr, "switch: %s closed in %.0f ms\n", cur->desc, now_ms() - t0);
	}
	cur = want;
	want = NULL;
	if (!cap.session && cam_open() < 0) {
		fprintf(stderr, "s24-cam: %s would not open; exiting\n", cur->desc);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	g_idle_add(submit_next, NULL);
	return G_SOURCE_REMOVE;
}

/* serve mode: an app started streaming from camera c: cancel the idle close, switch, run */
static void cam_wanted(Cam *c)
{
	if (screen_off)         /* resumed by screen_check when the screen lights */
		return;
	if (idle_timer) {
		g_source_remove(idle_timer);
		idle_timer = 0;
	}
	want = c;
	if (!switch_timer)
		try_switch(NULL);
}
#endif

/* shot/stream: one camera ID; serve: every ID given (one PipeWire node each) */
static int cmd_run(char **ids, int nids, const char *size, const char *out, int frames)
{
	if (sscanf(size, "%dx%d", &cap.width, &cap.height) != 2)
		return 2;
	cap.frames = frames;
	cap.out = out;
	cap.format = PIXEL_FORMAT_YCBCR_420_888;
	cap.alloc = service(ALLOC_IFACE "/default", ALLOC_IFACE);
	if (!cap.alloc)
		return 1;
	for (int i = 0; i < nids && ncams < MAXCAMS; i++) {
		Cam *c = &cams[ncams];
		c->id = ids[i];
		c->dev = camera_device(ids[i]);
		if (!c->dev)
			return 1;
		/* characteristics before open: cameraserver does this too; also gives facing and the
		 * sensor's mounting angle */
		gsize cl = 0;
		guint8 *cm = characteristics(c->dev, &cl);
		uint32_t n = 0;
		const guint8 *fac = meta_find(cm, cl, TAG_LENS_FACING, &n, NULL);
		const gint32 *ori = meta_find(cm, cl, TAG_SENSOR_ORIENTATION, &n, NULL);
		c->facing = fac ? *fac : 2;
		c->orient = ori ? *ori : 0;
		{       /* what face detection the camera offers (S24CAM_FACE uses SIMPLE) */
			uint32_t nf = 0, ns = 0, nm = 0;
			const guint8 *fdm = meta_find(cm, cl, TAG_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, &nf, NULL);
			const gint32 *mf = meta_find(cm, cl, TAG_STATISTICS_INFO_MAX_FACE_COUNT, &nm, NULL);
			const guint8 *sm = meta_find(cm, cl, TAG_CONTROL_AVAILABLE_SCENE_MODES, &ns, NULL);
			char a[64] = "", s[128] = "";
			for (uint32_t k = 0; fdm && k < nf && k < 8; k++)
				snprintf(a + strlen(a), sizeof(a) - strlen(a), " %u", fdm[k]);
			for (uint32_t k = 0; sm && k < ns && k < 24; k++)
				snprintf(s + strlen(s), sizeof(s) - strlen(s), " %u", sm[k]);
			fprintf(stderr, "camera %s: face detect modes [%s ], max faces %d, scene modes [%s ]\n",
			        ids[i], a, mf && nm ? mf[0] : -1, s);
		}
		g_free(cm);
		if (pw_mode) {          /* upright for a phone held upright, never mirrored (see pw_setup) */
			c->rot = c->orient;
			c->hflip = 0;
		} else {                /* shot/stream: env S24CAM_ROT (clockwise) + S24CAM_HFLIP */
			const char *r = getenv("S24CAM_ROT");
			c->rot = r ? atoi(r) : 0;
			c->hflip = getenv("S24CAM_HFLIP") ? 1 : 0;
		}
		int same = 0;
		for (int k = 0; k < ncams; k++)
			same += cams[k].facing == c->facing;
		static const char *nn[] = { "samsung_front", "samsung_rear", "samsung_ext" };
		static const char *dd[] = { "Front Camera", "Back Camera", "External Camera" };
		int f = c->facing >= 0 && c->facing < 3 ? c->facing : 2;
		snprintf(c->node, sizeof(c->node), same ? "%s%d" : "%s", nn[f], same + 1);
		snprintf(c->desc, sizeof(c->desc), same ? "%s %d" : "%s", dd[f], same + 1);
		fprintf(stderr, "camera %s: %s, sensor mounted at %d degrees\n", c->id, c->desc, c->orient);
		ncams++;
	}
	if (!ncams)
		return 2;
	cur = &cams[0];
	/* zoom: read the active array and crop its centre, to steer a logical camera off its primary
	 * (Wide) sensor onto a Tele/UW whose stream controller doesn't need the null m+464 node */
	if (getenv("S24CAM_ZOOM"))
		g_zoom = atof(getenv("S24CAM_ZOOM"));
	if (g_zoom > 1.01f) {
		gsize clen = 0;
		guint8 *cm = characteristics(cur->dev, &clen);
		uint32_t c = 0;
		const gint32 *aa = meta_find(cm, clen, TAG_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &c, NULL);
		if (aa && c >= 4) {
			gint32 w = (gint32)(aa[2] / g_zoom), h = (gint32)(aa[3] / g_zoom);
			g_crop[0] = aa[0] + (aa[2] - w) / 2;
			g_crop[1] = aa[1] + (aa[3] - h) / 2;
			g_crop[2] = w;
			g_crop[3] = h;
			fprintf(stderr, "zoom %.2fx: active array %dx%d -> crop [%d %d %d %d]\n", g_zoom,
			       aa[2], aa[3], g_crop[0], g_crop[1], g_crop[2], g_crop[3]);
		} else {
			fprintf(stderr, "zoom %.2fx requested but no ACTIVE_ARRAY_SIZE; using ZOOM_RATIO only\n", g_zoom);
		}
		g_free(cm);
	}
	cap.cb = gbinder_servicemanager_new_local_object(sm, CALLBACK_IFACE, cb_transact, NULL);
	gbinder_local_object_set_stability(cap.cb, GBINDER_STABILITY_VINTF);

	if (cam_open() < 0)         /* serve: proves the camera works; idle-closed again below */
		return 1;
	if (!stream_mode) { const char *t = getenv("S24CAM_TIMEOUT"); g_timeout_add_seconds(t ? atoi(t) : 30, on_timeout, NULL); }
#ifdef S24CAM_PIPEWIRE
	if (pw_mode) {
		if (pw_setup() < 0) {
			cam_close();
			return 1;
		}
		cam_idle_soon();        /* closes the camera again unless an app connects first */
		g_timeout_add_seconds(1, watchdog, NULL);
	}
#endif
	/* SIGTERM/SIGINT: leave the main loop and CLOSE the session (a killed client leaves the HAL
	 * to clean up after a binder death, and the next open then gets a half-set-up camera) */
	g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
	g_unix_signal_add(SIGINT, on_quit_signal, NULL);
	g_idle_add(submit_next, NULL);
	g_main_loop_run(loop);
	cam_close();
	return cap.saved || pw_mode ? 0 : 1;
}

static void on_provider_died(GBinderRemoteObject *obj, void *user)
{
	fprintf(stderr, "s24-cam: the camera provider died; exiting (it needs a full setup again)\n");
	_exit(3);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: s24-cam list | shot ID WxH OUT [FRAMES]\n");
		return 2;
	}
	sm = gbinder_servicemanager_new("/dev/binder");
	loop = g_main_loop_new(NULL, FALSE);
	int pst = 0;
	GBinderRemoteObject *po = sm ? gbinder_servicemanager_get_service_sync(sm,
	                                   PROVIDER_IFACE "/internal/0", &pst) : NULL;
	if (!po) {
		fprintf(stderr, "s24-cam: no camera provider (%d)\n", pst);
		return 1;
	}
	provider = gbinder_client_new(po, PROVIDER_IFACE);
	/* init restarts a crashed provider UNPRIMED, which no open can use: exit now so whoever
	 * started us can redo the whole setup, instead of failing later when an app connects */
	gbinder_remote_object_add_death_handler(po, on_provider_died, NULL);
	if (!getenv("S24CAM_NOPRIME")) {
		set_provider_callback();
		prime_vendor_tags();
	}
	if (!strcmp(argv[1], "list"))
		return cmd_list();
	if (!strcmp(argv[1], "shot") && argc >= 5)
		return cmd_run(&argv[2], 1, argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 30);
	if (!strcmp(argv[1], "stream") && argc >= 4) {
		stream_mode = 1;
		return cmd_run(&argv[2], 1, argv[3], "/dev/null", 1 << 30);
	}
	if (!strcmp(argv[1], "serve") && argc >= 4) {
#ifdef S24CAM_PIPEWIRE
		char **ids = g_strsplit(argv[2], ",", MAXCAMS);
		stream_mode = pw_mode = 1;
		return cmd_run(ids, g_strv_length(ids), argv[3], "/dev/null", 1 << 30);
#else
		fprintf(stderr, "s24-cam: built without PipeWire (add -DS24CAM_PIPEWIRE)\n");
		return 2;
#endif
	}
	fprintf(stderr, "usage: s24-cam list | shot ID WxH OUT [FRAMES] | stream ID WxH | serve ID[,ID..] WxH\n");
	return 2;
}
