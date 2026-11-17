// Galaxy S24 (SM-S921B) camera helper for postmarketOS (Claude, 2026-11-15).
//
// Samsung's camera HAL (vendor.samsung.hardware.camera.provider-service_64, AIDL
// android.hardware.camera.provider.ICameraProvider/internal/0) runs in the Android container
// (android-ctr0.sh start camera). It needs Android's sensor service
// (android.frameworks.sensorservice.ISensorManager/default, normally inside system_server): it
// waits for it at start, and its real-time image library (libIRTA) aborts the whole HAL when it
// can't enable a gyroscope. This daemon is that service, for the camera only: a gyroscope and an
// accelerometer that report a phone held still (no rotation, gravity along +Y), at the rate asked.
//
//   s24-camd        register the service (retrying until the container's service manager is up)
//
// Build on the phone: gcc -O2 -Wall -o s24-camd s24-camd.c $(pkg-config --cflags --libs libgbinder gobject-2.0)
// Binder setup: /etc/gbinder.conf (aidl3 for /dev/binder), as for s24-radio.
#include <gbinder.h>
#include <glib.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define AIDL_GET_VERSION 0x00ffffff
#define AIDL_GET_HASH 0x00fffffe
#define EX_SERVICE_SPECIFIC (-8)

/* android.frameworks.sensorservice V1 (frozen): codes in declaration order */
#define SS_HASH "514f08f7a595501066b94e5de878d240b4d49456"
#define SM_IFACE "android.frameworks.sensorservice.ISensorManager"
#define QUEUE_IFACE "android.frameworks.sensorservice.IEventQueue"
#define QCB_IFACE "android.frameworks.sensorservice.IEventQueueCallback"
enum { SM_CREATE_ASHMEM_DIRECT_CHANNEL = 1, SM_CREATE_EVENT_QUEUE, SM_CREATE_GRALLOC_DIRECT_CHANNEL,
       SM_GET_DEFAULT_SENSOR, SM_GET_SENSOR_LIST };
enum { Q_DISABLE_SENSOR = 1, Q_ENABLE_SENSOR };
enum { QCB_ON_EVENT = 1 };
#define RESULT_NOT_EXIST 1

/* android.hardware.sensors */
enum { TYPE_ACCELEROMETER = 1, TYPE_GYROSCOPE = 4 };
#define FLAG_CONTINUOUS 0

typedef struct {
	int type;
	const char *name, *type_string;
	float max_range, resolution, power;
	float x, y, z;          /* the reading: a phone held still */
} Sensor;
static const Sensor sensors[] = {
	{ TYPE_ACCELEROMETER, "s24 accelerometer (still)", "android.sensor.accelerometer",
	  156.9f, 0.0048f, 0.17f, 0.0f, 9.81f, 0.0f },
	{ TYPE_GYROSCOPE, "s24 gyroscope (still)", "android.sensor.gyroscope",
	  34.9f, 0.0012f, 0.55f, 0.0f, 0.0f, 0.0f },
};
#define NSENSORS (int)(sizeof(sensors) / sizeof(sensors[0]))

typedef struct {
	GBinderLocalObject *obj;
	GBinderClient *cb;      /* the client's IEventQueueCallback */
	GBinderRemoteObject *cbo;
	guint timer[NSENSORS];
	int id;
	gboolean held;          /* the client holds a strong reference to this queue */
} Queue;

/* libgbinder's private object layout (src/gbinder_local_object_p.h, 1.1.x): the reference
 * counts the binder driver asks us to keep for remote holders. Read-only use, to learn when the
 * client dropped its queue: the camera HAL drops two of its three queues on close WITHOUT
 * disabling their gyroscopes, and real Android tears a queue down when its holder lets go. */
typedef struct {
	GObject object;
	gpointer priv, ipc;
	const char *const *ifaces;
	gint weak_refs, strong_refs;
} LocalObjectView;

static GMainLoop *loop;
static GBinderServiceManager *sm;
static GPtrArray *queues;

static const Sensor *sensor_by_handle(int h, int *idx)
{
	for (int i = 0; i < NSENSORS; i++)
		if (sensors[i].type == h) {
			if (idx)
				*idx = i;
			return &sensors[i];
		}
	return NULL;
}

static void ok_reply(GBinderWriter *w)
{
	gbinder_writer_append_int32(w, 0);              /* EX_NONE */
}

static void not_exist_reply(GBinderWriter *w)
{
	/* ServiceSpecificException(RESULT_NOT_EXIST), as Status::writeToParcel lays it out */
	gbinder_writer_append_int32(w, EX_SERVICE_SPECIFIC);
	gbinder_writer_append_string16(w, "no such sensor");
	gbinder_writer_append_int32(w, 0);              /* empty remote stack trace header */
	gbinder_writer_append_int32(w, RESULT_NOT_EXIST);
}

static void write_sensor_info(GBinderWriter *w, const Sensor *s)
{
	GBinderWriter p;
	gbinder_writer_start_parcelable(w, &p);
	gbinder_writer_append_int32(&p, s->type);       /* sensorHandle = type */
	gbinder_writer_append_string16(&p, s->name);
	gbinder_writer_append_string16(&p, "postmarketOS");
	gbinder_writer_append_int32(&p, 1);             /* version */
	gbinder_writer_append_int32(&p, s->type);
	gbinder_writer_append_string16(&p, s->type_string);
	gbinder_writer_append_float(&p, s->max_range);
	gbinder_writer_append_float(&p, s->resolution);
	gbinder_writer_append_float(&p, s->power);
	gbinder_writer_append_int32(&p, 2500);          /* minDelayUs: 400 Hz */
	gbinder_writer_append_int32(&p, 0);             /* fifoReservedEventCount */
	gbinder_writer_append_int32(&p, 0);             /* fifoMaxEventCount */
	gbinder_writer_append_string16(&p, "");         /* requiredPermission */
	gbinder_writer_append_int32(&p, 1000000);       /* maxDelayUs */
	gbinder_writer_append_int32(&p, FLAG_CONTINUOUS);
	gbinder_writer_finish_parcelable(&p);
}

/* one Event, Vec3 payload (union tag 0), status ACCURACY_HIGH */
static gboolean send_event(gpointer data)
{
	Queue *q = ((gpointer *)data)[0];
	const Sensor *s = ((gpointer *)data)[1];
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	GBinderLocalRequest *req = gbinder_client_new_request(q->cb);
	GBinderWriter w, e, v;
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_start_parcelable(&w, &e);        /* Event */
	gbinder_writer_append_int64(&e, ts.tv_sec * 1000000000LL + ts.tv_nsec);
	gbinder_writer_append_int32(&e, s->type);       /* sensorHandle */
	gbinder_writer_append_int32(&e, s->type);       /* sensorType */
	gbinder_writer_append_int32(&e, 1);             /* payload union: non-null, tag 0 = vec3 */
	gbinder_writer_append_int32(&e, 0);
	gbinder_writer_start_parcelable(&e, &v);        /* Vec3 */
	gbinder_writer_append_float(&v, s->x);
	gbinder_writer_append_float(&v, s->y);
	gbinder_writer_append_float(&v, s->z);
	gbinder_writer_append_int32(&v, 3);             /* SensorStatus ACCURACY_HIGH (byte, padded) */
	gbinder_writer_finish_parcelable(&v);
	gbinder_writer_finish_parcelable(&e);
	/* straight ioctl on this thread: the async gbinder_client_transact() hands every event to a
	 * worker thread, which cost ~1 ms of CPU per event (a third of a core at 400 events/s) */
	gbinder_client_transact_sync_oneway(q->cb, QCB_ON_EVENT, req);
	gbinder_local_request_unref(req);
	return G_SOURCE_CONTINUE;
}

static void queue_stop(Queue *q, const char *why)
{
	for (int i = 0; i < NSENSORS; i++)
		if (q->timer[i]) {
			g_source_remove(q->timer[i]);
			q->timer[i] = 0;
			printf("s24-camd: queue %d: %s off (%s)\n", q->id, sensors[i].name, why);
		}
}

static void on_queue_refs(GObject *obj, gpointer user)
{
	Queue *q = user;
	const LocalObjectView *v = (const LocalObjectView *)obj;
	if (v->strong_refs > 0) {
		q->held = TRUE;
	} else if (q->held) {
		q->held = FALSE;
		queue_stop(q, "released by the camera");
	}
}

static void on_client_died(GBinderRemoteObject *obj, void *user)
{
	queue_stop(user, "camera process gone");
}

static GBinderLocalReply *queue_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                         guint code, guint flags, int *status, void *user)
{
	Queue *q = user;
	GBinderLocalReply *rep = gbinder_local_object_new_reply(obj);
	GBinderWriter w;
	GBinderReader r;
	gint32 handle = 0, period = 0;
	gint64 latency = 0;
	int i;
	gbinder_local_reply_init_writer(rep, &w);
	gbinder_remote_request_init_reader(req, &r);
	*status = 0;
	if (code == AIDL_GET_VERSION || code == AIDL_GET_HASH) {
		ok_reply(&w);
		if (code == AIDL_GET_VERSION)
			gbinder_writer_append_int32(&w, 1);
		else
			gbinder_writer_append_string16(&w, SS_HASH);
		return rep;
	}
	gbinder_reader_read_int32(&r, &handle);
	if (!sensor_by_handle(handle, &i)) {
		not_exist_reply(&w);
		return rep;
	}
	if (q->timer[i]) {
		g_source_remove(q->timer[i]);
		q->timer[i] = 0;
	}
	if (code == Q_ENABLE_SENSOR) {
		gbinder_reader_read_int32(&r, &period);
		gbinder_reader_read_int64(&r, &latency);
		int ms = MAX(period / 1000, 5);         /* at most 200 Hz of still readings */
		gpointer *data = g_new(gpointer, 2);
		data[0] = q;
		data[1] = (gpointer)&sensors[i];
		q->timer[i] = g_timeout_add_full(G_PRIORITY_DEFAULT, ms, send_event, data, g_free);
		printf("s24-camd: queue %d: %s on, every %d ms (asked %d us)\n", q->id, sensors[i].name, ms, period);
	} else {
		printf("s24-camd: queue %d: %s off\n", q->id, sensors[i].name);
	}
	ok_reply(&w);
	return rep;
}

static GBinderLocalReply *sensor_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                          guint code, guint flags, int *status, void *user)
{
	GBinderLocalReply *rep = gbinder_local_object_new_reply(obj);
	GBinderWriter w;
	GBinderReader r;
	gbinder_local_reply_init_writer(rep, &w);
	gbinder_remote_request_init_reader(req, &r);
	*status = 0;
	switch (code) {
	case AIDL_GET_VERSION:
		ok_reply(&w);
		gbinder_writer_append_int32(&w, 1);
		break;
	case AIDL_GET_HASH:
		ok_reply(&w);
		gbinder_writer_append_string16(&w, SS_HASH);
		break;
	case SM_GET_SENSOR_LIST:
		ok_reply(&w);
		gbinder_writer_append_int32(&w, NSENSORS);
		for (int i = 0; i < NSENSORS; i++)
			write_sensor_info(&w, &sensors[i]);
		printf("s24-camd: sensor list asked\n");
		break;
	case SM_GET_DEFAULT_SENSOR: {
		gint32 type = 0;
		gbinder_reader_read_int32(&r, &type);
		const Sensor *s = sensor_by_handle(type, NULL);
		if (s) {
			ok_reply(&w);
			write_sensor_info(&w, s);
		} else {
			not_exist_reply(&w);
		}
		printf("s24-camd: default sensor of type %d: %s\n", type, s ? s->name : "none");
		break;
	}
	case SM_CREATE_EVENT_QUEUE: {
		GBinderRemoteObject *cbo = gbinder_reader_read_object(&r);
		if (!cbo) {
			not_exist_reply(&w);
			break;
		}
		Queue *q = g_new0(Queue, 1);
		q->cb = gbinder_client_new(cbo, QCB_IFACE);
		q->cbo = cbo;           /* keep: death notice when the camera process goes */
		gbinder_remote_object_add_death_handler(cbo, on_client_died, q);
		q->obj = gbinder_servicemanager_new_local_object(sm, QUEUE_IFACE, queue_transact, q);
		gbinder_local_object_set_stability(q->obj, GBINDER_STABILITY_VINTF);
		g_ptr_array_add(queues, q);
		q->id = queues->len;
		if (g_signal_lookup("strong_refs_changed", G_OBJECT_TYPE(q->obj)))
			g_signal_connect(q->obj, "strong_refs_changed", G_CALLBACK(on_queue_refs), q);
		else
			printf("s24-camd: no strong_refs_changed signal: queues stop only on disable/death\n");
		ok_reply(&w);
		gbinder_writer_append_local_object(&w, q->obj);
		printf("s24-camd: event queue %d created\n", q->id);
		break;
	}
	default:
		not_exist_reply(&w);
		printf("s24-camd: call %u: not available\n", code);
	}
	return rep;
}

static gboolean on_signal(gpointer unused)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	queues = g_ptr_array_new();
	GBinderLocalObject *svc = NULL;
	/* the container's service manager may not be up yet: a connection made before it was is
	 * useless, so start over each time */
	for (int tries = 0;; tries++) {
		sm = gbinder_servicemanager_new("/dev/binder");
		int st = -1;
		if (sm) {
			svc = gbinder_servicemanager_new_local_object(sm, SM_IFACE, sensor_transact, NULL);
			/* the HAL is vendor code: it only talks to VINTF-stable binders */
			gbinder_local_object_set_stability(svc, GBINDER_STABILITY_VINTF);
			st = gbinder_servicemanager_add_service_sync(sm, SM_IFACE "/default", svc);
			if (st == 0)
				break;
			gbinder_local_object_unref(svc);
			gbinder_servicemanager_unref(sm);
			svc = NULL;
			sm = NULL;
		}
		if (tries == 60) {
			fprintf(stderr, "s24-camd: could not register %s/default (%d)\n", SM_IFACE, st);
			return 1;
		}
		g_usleep(G_USEC_PER_SEC);
	}
	printf("s24-camd: %s/default registered (still gyroscope + accelerometer)\n", SM_IFACE);
	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_main_loop_run(loop);
	return 0;
}
