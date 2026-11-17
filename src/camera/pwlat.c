// pwlat: measure how long a camera frame takes from s24-cam to an app over PipeWire.
// s24-cam (env S24CAM_STAMP=1) writes its hand-off time (CLOCK_MONOTONIC, microseconds) into the
// first 8 bytes of every frame; this consumer compares it with the time the frame reaches it.
//   usage: pwlat [target-node] [seconds]      (run in the user's session: XDG_RUNTIME_DIR)
//   build: gcc -O2 -Wall -o pwlat pwlat.c $(pkg-config --cflags --libs libpipewire-0.3)
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	double sum, max, all_sum, t_last, t_start;
	int n, total, all_n;
};

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void on_process(void *userdata)
{
	struct data *d = userdata;
	struct pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
	if (!b)
		return;
	struct spa_data *sd = &b->buffer->datas[0];
	if (sd->data && sd->chunk->size >= 8) {
		uint64_t stamp;
		memcpy(&stamp, (uint8_t *)sd->data + sd->chunk->offset, sizeof(stamp));
		double lat = now_us() - (double)stamp;
		if (!d->total++)
			printf("[pwlat] first frame %.0f ms after connecting\n", (now_us() - d->t_start) / 1000);
		if (lat > 0 && lat < 5e6) {
			d->sum += lat;
			d->all_sum += lat;
			d->all_n++;
			if (lat > d->max)
				d->max = lat;
			if (++d->n == 30) {
				double t = now_us();
				printf("[pwlat] s24-cam -> app: avg %.1f ms, max %.1f ms | %.1f fps\n",
				       d->sum / d->n / 1000, d->max / 1000, 30e6 / (t - d->t_last));
				fflush(stdout);
				d->t_last = t;
				d->sum = d->max = 0;
				d->n = 0;
			}
		}
	}
	pw_stream_queue_buffer(d->stream, b);
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct spa_video_info_raw info;
	if (param && id == SPA_PARAM_Format && spa_format_video_raw_parse(param, &info) >= 0)
		printf("[pwlat] format %ux%u (spa video format %u)\n", info.size.width, info.size.height,
		       info.format);
}

static void on_state(void *userdata, enum pw_stream_state old, enum pw_stream_state st, const char *err)
{
	printf("[pwlat] %s -> %s %s\n", pw_stream_state_as_string(old), pw_stream_state_as_string(st),
	       err ? err : "");
}

static const struct pw_stream_events events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void on_timeout(void *userdata, uint64_t expirations)
{
	pw_main_loop_quit(((struct data *)userdata)->loop);
}

int main(int argc, char **argv)
{
	struct data d = { 0 };
	const char *target = argc > 1 ? argv[1] : "samsung_rear";
	int secs = argc > 2 ? atoi(argv[2]) : 10;
	pw_init(&argc, &argv);
	d.loop = pw_main_loop_new(NULL);
	struct pw_loop *l = pw_main_loop_get_loop(d.loop);
	d.stream = pw_stream_new_simple(l, "pwlat",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
		                  PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_TARGET_OBJECT, target, NULL),
		&events, &d);
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(1080, 1920),
			&SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(30, 1),
			&SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
	d.t_start = now_us();
	pw_stream_connect(d.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
	                  PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	struct spa_source *timer = pw_loop_add_timer(l, on_timeout, &d);
	struct timespec ts = { secs, 0 };
	pw_loop_update_timer(l, timer, &ts, NULL, false);
	d.t_last = now_us();
	pw_main_loop_run(d.loop);
	printf("[pwlat] %d frames, overall avg %.1f ms\n", d.total, d.all_n ? d.all_sum / d.all_n / 1000 : -1.0);
	pw_stream_destroy(d.stream);
	pw_main_loop_destroy(d.loop);
	return 0;
}
