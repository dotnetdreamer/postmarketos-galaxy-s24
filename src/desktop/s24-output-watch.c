/*
 * s24-output-watch: the session's screens, for the S24's mouse mode (Claude, 2026-11-15).
 * A Wayland client that prints one line per screen whenever the compositor has told it
 * everything about that screen, and one when a screen goes:
 *   output <name> <transform> <width>x<height>     (wl_output.done)
 *   dpms <name> <mode>                             (KWin's org_kde_kwin_dpms: 0 on, 1 standby,
 *                                                   2 suspend, 3 off)
 *   gone <name>
 *   ready                                          (once, after the screens there at the start)
 * <transform> is wl_output's: 0 normal, 1 90, 2 180, 3 270 (4-7 flipped).
 * It ends when the compositor goes away.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wayland-client.h>
#include "dpms-client-protocol.h"

struct screen {
    struct wl_output *output;
    struct org_kde_kwin_dpms *dpms;
    uint32_t global;
    char name[64];
    int transform, width, height;
    unsigned dpms_mode;
    struct screen *next;
};
static struct screen *screens;
static struct org_kde_kwin_dpms_manager *dpms_manager;

static void geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                     int32_t subpixel, const char *make, const char *model, int32_t transform)
{
    ((struct screen *)data)->transform = transform;
}

static void mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    struct screen *s = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        s->width = w;
        s->height = h;
    }
}

static void done(void *data, struct wl_output *o)
{
    struct screen *s = data;
    printf("output %s %d %dx%d\n", s->name, s->transform, s->width, s->height);
    fflush(stdout);
}

static void scale(void *data, struct wl_output *o, int32_t factor) {}

static void name(void *data, struct wl_output *o, const char *n)
{
    struct screen *s = data;
    snprintf(s->name, sizeof s->name, "%s", n);
}

static void description(void *data, struct wl_output *o, const char *d) {}

static const struct wl_output_listener output_listener = {
    .geometry = geometry, .mode = mode, .done = done, .scale = scale,
    .name = name, .description = description,
};

static void dpms_supported(void *data, struct org_kde_kwin_dpms *d, uint32_t supported) {}

static void dpms_mode(void *data, struct org_kde_kwin_dpms *d, uint32_t m)
{
    ((struct screen *)data)->dpms_mode = m;
}

static void dpms_done(void *data, struct org_kde_kwin_dpms *d)
{
    struct screen *s = data;
    printf("dpms %s %u\n", s->name, s->dpms_mode);
    fflush(stdout);
}

static const struct org_kde_kwin_dpms_listener dpms_listener = {
    .supported = dpms_supported, .mode = dpms_mode, .done = dpms_done,
};

static void watch_dpms(struct screen *s)
{
    if (!dpms_manager || s->dpms) return;
    s->dpms = org_kde_kwin_dpms_manager_get(dpms_manager, s->output);
    org_kde_kwin_dpms_add_listener(s->dpms, &dpms_listener, s);
}

static void global(void *data, struct wl_registry *reg, uint32_t id, const char *iface, uint32_t version)
{
    if (!strcmp(iface, org_kde_kwin_dpms_manager_interface.name)) {
        dpms_manager = wl_registry_bind(reg, id, &org_kde_kwin_dpms_manager_interface, 1);
        for (struct screen *s = screens; s; s = s->next) watch_dpms(s);
        return;
    }
    if (strcmp(iface, wl_output_interface.name)) return;
    struct screen *s = calloc(1, sizeof *s);
    if (!s) return;
    s->global = id;
    snprintf(s->name, sizeof s->name, "output-%u", id);
    s->output = wl_registry_bind(reg, id, &wl_output_interface, version < 4 ? version : 4);
    wl_output_add_listener(s->output, &output_listener, s);
    s->next = screens;
    screens = s;
    watch_dpms(s);
}

static void global_remove(void *data, struct wl_registry *reg, uint32_t id)
{
    for (struct screen **p = &screens; *p; p = &(*p)->next) {
        if ((*p)->global != id) continue;
        struct screen *s = *p;
        printf("gone %s\n", s->name);
        fflush(stdout);
        *p = s->next;
        if (s->dpms) org_kde_kwin_dpms_release(s->dpms);
        wl_output_destroy(s->output);
        free(s);
        return;
    }
}

static const struct wl_registry_listener registry_listener = { .global = global, .global_remove = global_remove };

int main(void)
{
    /* it lives for the agent that reads it: gone with it, even when that one is killed */
    pid_t parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) return 0;
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        fprintf(stderr, "s24-output-watch: no Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    printf("ready\n");
    fflush(stdout);
    while (wl_display_dispatch(d) != -1) {
    }
    return 0;
}
