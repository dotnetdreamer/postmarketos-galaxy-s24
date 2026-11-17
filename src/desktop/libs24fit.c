/* libs24fit.so (Galaxy S24, postmarketOS; Claude, 2026-11-16): preloaded into the nested
 * kwin_wayland that `s24-fit run` starts for an app too wide for the phone screen. That KWin shows
 * its one output as a window on the phone's KWin and takes the output's size and scale from it
 * (wayland_output.cpp: the size from xdg_toplevel.configure, the scale from
 * wp_fractional_scale_v1.preferred_scale; any other scale it ignores on purpose). This library sits
 * in libwayland-client's calls between the two and makes the nested KWin believe its window is 1/F
 * times bigger and the phone's scale F times smaller: it renders the same pixels (full sharpness),
 * but its output, and the app on it, is 1/F times wider in logical px. Going out, what it shows is
 * shrunk back by F (the viewport destination and every other surface coordinate); coming in,
 * touch and pointer positions are grown by 1/F. It also gives the window the app's title and app
 * id, so the phone's task switcher and app drawer know it as that app, and passes on a close request
 * (swiping it away in the task switcher), which the nested KWin itself ignores: SIGUSR1 to s24-fit,
 * which asks the nested KWin to close the app's windows.
 * Env, read once and then removed so the KWin's children (its on-screen keyboard) don't get it:
 *   S24_FIT_F       the factor, 0.1..1 (phone size = nested size * F)
 *   S24_FIT_TITLE   window title for the phone's KWin
 *   S24_FIT_APP_ID  app id for the phone's KWin (a .desktop file name without .desktop)
 *   S24_FIT_NOTIFY  pid to send SIGUSR1 when the phone's KWin asks the window to close
 *   S24_FIT_PRELOAD LD_PRELOAD to restore for the children (unset when empty)
 * Build: gcc -O2 -shared -fPIC -o libs24fit.so libs24fit.c -lwayland-client -lm */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wayland-client-core.h>

#define MAX_ARGS 20 /* WL_CLOSURE_MAX_ARGS */

static double F = 1;
static char *title, *app_id;
static pid_t notify_pid;

__attribute__((constructor)) static void init(void)
{
	const char *f = getenv("S24_FIT_F"), *t = getenv("S24_FIT_TITLE"), *a = getenv("S24_FIT_APP_ID");
	const char *n = getenv("S24_FIT_NOTIFY"), *p = getenv("S24_FIT_PRELOAD");

	if (f && strtod(f, NULL) >= 0.1 && strtod(f, NULL) < 1)
		F = strtod(f, NULL);
	if (t && *t)
		title = strdup(t);
	if (a && *a)
		app_id = strdup(a);
	if (n && atoi(n) > 1)
		notify_pid = (pid_t)atoi(n);
	if (p && *p)
		setenv("LD_PRELOAD", p, 1);
	else
		unsetenv("LD_PRELOAD");
	unsetenv("S24_FIT_F");
	unsetenv("S24_FIT_TITLE");
	unsetenv("S24_FIT_APP_ID");
	unsetenv("S24_FIT_NOTIFY");
	unsetenv("S24_FIT_PRELOAD");
}

/* struct wl_proxy starts with struct wl_object, whose first member is its interface */
static const struct wl_interface *iface_of(struct wl_proxy *p)
{
	return *(const struct wl_interface **)p;
}

/* ---- events: the original listeners, per proxy (newest first: addresses get reused) ---- */
struct hook {
	void *proxy;
	void (**orig)(void);
	struct hook *next;
};
static struct hook *hooks;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void (**orig_of(void *proxy))(void)
{
	void (**o)(void) = NULL;

	pthread_mutex_lock(&lock);
	for (struct hook *h = hooks; h; h = h->next)
		if (h->proxy == proxy) {
			o = h->orig;
			break;
		}
	pthread_mutex_unlock(&lock);
	return o;
}

static wl_fixed_t grow(wl_fixed_t v)
{
	return wl_fixed_from_double(wl_fixed_to_double(v) / F);
}

static int32_t grow_size(int32_t v)
{
	return v > 0 ? (int32_t)lround(v / F) : v;
}

typedef void (*ptr_enter_t)(void *, void *, uint32_t, void *, wl_fixed_t, wl_fixed_t);
static void ptr_enter(void *d, void *p, uint32_t serial, void *s, wl_fixed_t x, wl_fixed_t y)
{
	((ptr_enter_t)orig_of(p)[0])(d, p, serial, s, grow(x), grow(y));
}

typedef void (*ptr_motion_t)(void *, void *, uint32_t, wl_fixed_t, wl_fixed_t);
static void ptr_motion(void *d, void *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	((ptr_motion_t)orig_of(p)[2])(d, p, time, grow(x), grow(y));
}

typedef void (*touch_down_t)(void *, void *, uint32_t, uint32_t, void *, int32_t, wl_fixed_t, wl_fixed_t);
static void touch_down(void *d, void *p, uint32_t serial, uint32_t time, void *s, int32_t id, wl_fixed_t x,
		       wl_fixed_t y)
{
	((touch_down_t)orig_of(p)[0])(d, p, serial, time, s, id, grow(x), grow(y));
}

typedef void (*touch_motion_t)(void *, void *, uint32_t, int32_t, wl_fixed_t, wl_fixed_t);
static void touch_motion(void *d, void *p, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	((touch_motion_t)orig_of(p)[2])(d, p, time, id, grow(x), grow(y));
}

typedef void (*configure_t)(void *, void *, int32_t, int32_t, struct wl_array *);
static void top_configure(void *d, void *p, int32_t w, int32_t h, struct wl_array *states)
{
	((configure_t)orig_of(p)[0])(d, p, grow_size(w), grow_size(h), states);
}

/* the nested KWin answers a close request by quitting: it takes its D-Bus objects down first, then
 * waits for its on-screen keyboard, which can hang on the way out, so the app never even notices.
 * With s24-fit to tell, it doesn't see the request: s24-fit closes the app's windows (the app can
 * still ask about unsaved work) and ends the KWin once the app has gone */
typedef void (*close_t)(void *, void *);
static void top_close(void *d, void *p)
{
	if (notify_pid > 0 && kill(notify_pid, SIGUSR1) == 0)
		return;
	((close_t)orig_of(p)[1])(d, p);
}

typedef void (*bounds_t)(void *, void *, int32_t, int32_t);
static void top_bounds(void *d, void *p, int32_t w, int32_t h)
{
	((bounds_t)orig_of(p)[2])(d, p, grow_size(w), grow_size(h));
}

typedef void (*scale_t)(void *, void *, uint32_t);
static void preferred_scale(void *d, void *p, uint32_t scale120)
{
	((scale_t)orig_of(p)[0])(d, p, (uint32_t)lround(scale120 * F));
}

static const struct {
	const char *cls;
	int event;
	void (*fn)(void);
} event_hooks[] = {
	{"xdg_toplevel", 1, (void (*)(void))top_close}, /* first: the only one used at F 1 */
	{"wl_pointer", 0, (void (*)(void))ptr_enter},
	{"wl_pointer", 2, (void (*)(void))ptr_motion},
	{"wl_touch", 0, (void (*)(void))touch_down},
	{"wl_touch", 2, (void (*)(void))touch_motion},
	{"xdg_toplevel", 0, (void (*)(void))top_configure},
	{"xdg_toplevel", 2, (void (*)(void))top_bounds},
	{"wp_fractional_scale_v1", 0, (void (*)(void))preferred_scale},
};

/* how many events this proxy's version has (a listener struct holds at least those) */
static int event_count(struct wl_proxy *p)
{
	const struct wl_interface *i = iface_of(p);
	uint32_t v = wl_proxy_get_version(p);
	int n = 0;

	for (int e = 0; e < i->event_count; e++) {
		uint32_t since = (uint32_t)strtoul(i->events[e].signature, NULL, 10);
		if ((since ? since : 1) <= (v ? v : 1))
			n = e + 1;
	}
	return n;
}

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data)
{
	static int (*real)(struct wl_proxy *, void (**)(void), void *);
	void (**mine)(void) = NULL;
	const char *cls;
	int n;

	if (!real)
		real = (int (*)(struct wl_proxy *, void (**)(void), void *))dlsym(RTLD_NEXT, "wl_proxy_add_listener");
	if ((F == 1 && notify_pid <= 0) || !impl)
		return real(proxy, impl, data);
	cls = wl_proxy_get_class(proxy);
	n = event_count(proxy);
	for (size_t k = 0; k < sizeof event_hooks / sizeof *event_hooks; k++) {
		if (strcmp(cls, event_hooks[k].cls) || event_hooks[k].event >= n || !impl[event_hooks[k].event])
			continue;
		if (k > 0 && F == 1)
			continue;
		if (k == 0 && notify_pid <= 0)
			continue;
		if (!mine) {
			mine = malloc(n * sizeof *mine);
			if (!mine)
				return real(proxy, impl, data);
			memcpy(mine, impl, n * sizeof *mine);
		}
		mine[event_hooks[k].event] = event_hooks[k].fn;
	}
	if (!mine)
		return real(proxy, impl, data);
	struct hook *h = malloc(sizeof *h);
	if (!h) {
		free(mine);
		return real(proxy, impl, data);
	}
	h->proxy = proxy;
	h->orig = impl;
	pthread_mutex_lock(&lock);
	h->next = hooks;
	hooks = h;
	pthread_mutex_unlock(&lock);
	return real(proxy, mine, data);
}

/* ---- requests ---- */
static int32_t shrink(int32_t v)
{
	return (int32_t)lround(v * F);
}

static void rewrite(struct wl_proxy *proxy, uint32_t op, union wl_argument *a)
{
	const char *c = wl_proxy_get_class(proxy);

	if (!strcmp(c, "xdg_toplevel")) {
		if (op == 2 && title) /* set_title */
			a[0].s = title;
		else if (op == 3 && app_id) /* set_app_id */
			a[0].s = app_id;
		else if (F != 1 && (op == 7 || op == 8)) { /* set_max_size, set_min_size */
			a[0].i = shrink(a[0].i);
			a[1].i = shrink(a[1].i);
		} else if (F != 1 && op == 4) { /* show_window_menu */
			a[2].i = shrink(a[2].i);
			a[3].i = shrink(a[3].i);
		}
		return;
	}
	if (F == 1)
		return;
	if (!strcmp(c, "wp_viewport")) {
		if (op == 2 && a[0].i > 0 && a[1].i > 0) { /* set_destination */
			a[0].i = shrink(a[0].i);
			a[1].i = shrink(a[1].i);
		}
	} else if (!strcmp(c, "wl_surface")) {
		if (op == 1 || op == 10) { /* attach (buffer, x, y), offset (x, y) */
			int k = op == 1;
			a[k].i = shrink(a[k].i);
			a[k + 1].i = shrink(a[k + 1].i);
		} else if (op == 2) { /* damage, in surface coordinates: round outwards */
			a[0].i = (int32_t)floor(a[0].i * F);
			a[1].i = (int32_t)floor(a[1].i * F);
			a[2].i = (int32_t)ceil(a[2].i * F) + 1;
			a[3].i = (int32_t)ceil(a[3].i * F) + 1;
		}
	} else if (!strcmp(c, "wl_region")) {
		if (op == 1 || op == 2) /* add, subtract */
			for (int k = 0; k < 4; k++)
				a[k].i = shrink(a[k].i);
	} else if (!strcmp(c, "xdg_surface")) {
		if (op == 3) /* set_window_geometry */
			for (int k = 0; k < 4; k++)
				a[k].i = shrink(a[k].i);
	} else if (!strcmp(c, "wl_pointer")) {
		if (op == 0) { /* set_cursor: the hotspot */
			a[2].i = shrink(a[2].i);
			a[3].i = shrink(a[3].i);
		}
	}
}

/* what libwayland's own wl_proxy_marshal_flags does (wl_argument_from_va_list), plus rewrite() */
struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface,
					uint32_t version, uint32_t flags, ...)
{
	union wl_argument args[MAX_ARGS];
	const char *sig = iface_of(proxy)->methods[opcode].signature;
	va_list ap;
	int n = 0;

	va_start(ap, flags);
	for (const char *s = sig; *s && n < MAX_ARGS; s++) {
		switch (*s) {
		case 'i':
			args[n++].i = va_arg(ap, int32_t);
			break;
		case 'u':
			args[n++].u = va_arg(ap, uint32_t);
			break;
		case 'f':
			args[n++].f = va_arg(ap, wl_fixed_t);
			break;
		case 's':
			args[n++].s = va_arg(ap, const char *);
			break;
		case 'o':
		case 'n':
			args[n++].o = va_arg(ap, struct wl_object *);
			break;
		case 'a':
			args[n++].a = va_arg(ap, struct wl_array *);
			break;
		case 'h':
			args[n++].h = va_arg(ap, int32_t);
			break;
		}
	}
	va_end(ap);
	if (F != 1 || title || app_id)
		rewrite(proxy, opcode, args);
	struct wl_proxy *made = wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
	/* the nested KWin never sets an app id (the phone's KWin would call its window "kwin_wayland"):
	 * give it one as soon as its toplevel exists, before the first commit maps it */
	if (made && app_id && opcode == 1 && !strcmp(wl_proxy_get_class(proxy), "xdg_surface"))
		wl_proxy_marshal_flags(made, 3, NULL, wl_proxy_get_version(made), 0, app_id);
	return made;
}
