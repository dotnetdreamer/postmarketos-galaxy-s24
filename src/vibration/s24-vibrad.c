// Galaxy S24 postmarketOS: a vibration bridge for feedbackd (Claude, 2026-11-17).
//
// feedbackd (phosh/Plasma haptics) only drives a force-feedback device that advertises FF_RUMBLE.
// The S24's motor (Cirrus cs40l26, input device "sec_vibrator_inputff") advertises FF_PERIODIC,
// FF_GAIN and FF_CUSTOM but NOT FF_RUMBLE, so feedbackd rejects it ("No rumble capable vibra
// device"). This daemon creates a virtual input device "s24-vibra" that DOES advertise FF_RUMBLE,
// which feedbackd binds, and forwards every effect to the real motor as a periodic (sine) effect:
//   feedbackd --(FF_RUMBLE on the virtual uinput device)--> s24-vibrad --(FF_PERIODIC)--> cs40l26
//
// A udev rule tags "s24-vibra" (not the real device) as the feedbackd vibra device.
// Build (on the phone): gcc -O2 -Wall -o s24-vibrad s24-vibrad.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXEFF 16

static int realfd = -1;            /* the cs40l26 motor */
static int ufd = -1;               /* the virtual device we expose to feedbackd */
static int map[MAXEFF];            /* virtual effect id -> real effect id (or -1) */

static int open_motor(void)
{
	glob_t g;
	if (glob("/sys/class/input/event*", 0, NULL, &g))
		return -1;
	int fd = -1;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char p[320], name[64] = "";
		snprintf(p, sizeof(p), "%s/device/name", g.gl_pathv[i]);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (strcmp(name, "sec_vibrator_inputff") == 0) {
			const char *ev = strrchr(g.gl_pathv[i], '/');
			char dev[64];
			snprintf(dev, sizeof(dev), "/dev/input%s", ev);
			fd = open(dev, O_RDWR | O_CLOEXEC);
			break;
		}
	}
	globfree(&g);
	return fd;
}

/* set the motor's force-feedback gain to a level (0..0xffff) */
static void set_gain(int fd, int level)
{
	struct input_event ev = { .type = EV_FF, .code = FF_GAIN, .value = level };
	if (write(fd, &ev, sizeof(ev)) < 0)
		; /* best effort */
}

/* translate a rumble effect from feedbackd into a periodic sine on the motor; returns its id or -1 */
static int upload_real(const struct ff_effect *in)
{
	struct ff_effect e;
	memset(&e, 0, sizeof(e));
	e.type = FF_PERIODIC;
	e.id = -1;
	e.u.periodic.waveform = FF_SINE;
	e.u.periodic.period = 50;                                   /* ms */
	int mag = in->u.rumble.strong_magnitude;                    /* 0..0xffff */
	if (mag == 0)
		mag = in->u.rumble.weak_magnitude;
	e.u.periodic.magnitude = mag >> 1;                          /* periodic magnitude is s16 */
	e.replay.length = in->replay.length;
	e.replay.delay = in->replay.delay;
	if (ioctl(realfd, EVIOCSFF, &e) < 0) {
		fprintf(stderr, "s24-vibrad: upload periodic (mag %d) failed: %s\n",
		        e.u.periodic.magnitude, strerror(errno));
		return -1;
	}
	fprintf(stderr, "s24-vibrad: feedbackd effect -> motor periodic id %d, mag %d, %d ms\n",
	        e.id, e.u.periodic.magnitude, e.replay.length);
	return e.id;
}

static void play_real(int rid, int value)
{
	struct input_event ev = { .type = EV_FF, .code = (unsigned short)rid, .value = value };
	if (write(realfd, &ev, sizeof(ev)) < 0)
		; /* best effort */
	fprintf(stderr, "s24-vibrad: motor %s effect id %d\n", value ? "PLAY" : "stop", rid);
}

static void erase_real(int rid)
{
	if (rid >= 0)
		ioctl(realfd, EVIOCRMFF, rid);
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 0; i < MAXEFF; i++)
		map[i] = -1;

	realfd = open_motor();
	if (realfd < 0) {
		fprintf(stderr, "s24-vibrad: no sec_vibrator_inputff motor: %s\n", strerror(errno));
		return 1;
	}
	set_gain(realfd, 0xffff);

	ufd = open("/dev/uinput", O_RDWR | O_CLOEXEC);
	if (ufd < 0) {
		fprintf(stderr, "s24-vibrad: /dev/uinput: %s\n", strerror(errno));
		return 1;
	}
	ioctl(ufd, UI_SET_EVBIT, EV_FF);
	ioctl(ufd, UI_SET_FFBIT, FF_RUMBLE);
	ioctl(ufd, UI_SET_FFBIT, FF_PERIODIC);
	ioctl(ufd, UI_SET_FFBIT, FF_GAIN);
	struct uinput_setup us;
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = 0x1209;
	us.id.product = 0x5245;
	us.id.version = 1;
	us.ff_effects_max = MAXEFF;
	strcpy(us.name, "s24-vibra");
	if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "s24-vibrad: create virtual device: %s\n", strerror(errno));
		return 1;
	}
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	printf("s24-vibrad: virtual vibra up, forwarding rumble -> periodic to the motor\n");

	while (!stop) {
		struct input_event ev;
		ssize_t n = read(ufd, &ev, sizeof(ev));
		if (n != (ssize_t)sizeof(ev)) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			fprintf(stderr, "s24-vibrad: read ended (n=%zd, %s)\n", n, strerror(errno));
			break;
		}
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up;
			memset(&up, 0, sizeof(up));
			up.request_id = ev.value;
			if (ioctl(ufd, UI_BEGIN_FF_UPLOAD, &up) < 0)
				continue;
			int slot = up.effect.id;
			if (slot < 0 || slot >= MAXEFF) {         /* new effect: find a free slot */
				slot = -1;
				for (int i = 0; i < MAXEFF; i++)
					if (map[i] < 0) { slot = i; break; }
			} else if (map[slot] >= 0) {              /* replacing one: drop the old real effect */
				erase_real(map[slot]);
				map[slot] = -1;
			}
			if (slot < 0) {
				up.retval = -1;
			} else {
				int rid = upload_real(&up.effect);
				up.effect.id = slot;                  /* hand the slot back to feedbackd */
				map[slot] = rid;
				up.retval = rid >= 0 ? 0 : -1;
			}
			ioctl(ufd, UI_END_FF_UPLOAD, &up);
		} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
			struct uinput_ff_erase er;
			memset(&er, 0, sizeof(er));
			er.request_id = ev.value;
			if (ioctl(ufd, UI_BEGIN_FF_ERASE, &er) < 0)
				continue;
			if (er.effect_id < MAXEFF) {
				erase_real(map[er.effect_id]);
				map[er.effect_id] = -1;
			}
			er.retval = 0;
			ioctl(ufd, UI_END_FF_ERASE, &er);
		} else if (ev.type == EV_FF) {
			if (ev.code == FF_GAIN)
				set_gain(realfd, ev.value);
			else if (ev.code < MAXEFF && map[ev.code] >= 0)
				play_real(map[ev.code], ev.value);
		}
	}
	ioctl(ufd, UI_DEV_DESTROY);
	printf("s24-vibrad: stopped\n");
	return 0;
}
