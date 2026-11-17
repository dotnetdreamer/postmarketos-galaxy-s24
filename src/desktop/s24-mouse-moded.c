/*
 * s24-mouse-moded: the root half of mouse mode on the Galaxy S24 (postmarketOS, Plasma Mobile).
 * Claude, 2026-11-15.
 *
 * While mouse mode is on, this takes the touchscreen (EVIOCGRAB) and sends its touches on as a
 * virtual touchpad (uinput). libinput and KWin then treat the screen as a laptop touchpad: one
 * finger moves the pointer, a tap clicks, two fingers scroll, a two-finger tap is a right click,
 * a three-finger pinch is KWin's zoom. The session half (s24-mouse-mode, run as the user) does
 * the rest: the bigger desktop, KWin's zoom following the pointer, the keyboard.
 *
 * Two gestures of its own, decided before a touch reaches the touchpad:
 *   a swipe in from the top or bottom edge   -> "edge top|bottom" (the phone's own gestures)
 *   three fingers held still for 0.8 s       -> mouse mode ends
 * The edges follow the screen's rotation ("rotate").
 *
 * It never holds the touchscreen while the panel is dark: s24-powerd listens there for the
 * double-tap KEY_WAKEUP, and a grab would hide it from it. Mouse mode ends when the panel goes
 * dark: the session half hears it from KWin at once (DPMS), and this reads it itself too, the way
 * s24-powerd does (the DSI-1 connector's "state:" in the exynos DRM debugfs dump, or
 * card1-DSI-1/enabled = disabled; card1-DSI-1/dpms does not follow KWin's blanking on this
 * driver). The dump takes the display's locks, so it is read only once no touch has come for a
 * second, once a second. Grabs are taken and let go only while no finger is down, so KWin never
 * loses the end of a touch it saw begin.
 *
 * Control: a stream socket, /run/s24-mouse-mode.sock, one line per command:
 *   on | off | pause | resume | rotate <0-3> | status
 * and one line per event back, to every client:
 *   state off|on|paused        after every change
 *   exit hold|dark|error       it ended mouse mode itself
 *   edge top|bottom            a swipe in from that edge
 * Only root and the user who runs kwin_wayland may connect. When the client that turned mouse
 * mode on goes away while it is on, mouse mode ends and `s24-mouse-mode cleanup` runs as that
 * user with that client's environment, so a session is never left zoomed on a plain touchscreen.
 *
 * Testing: --device /dev/input/eventN (a stand-in touchscreen), --dpms none|<file>, -v.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TOUCH_NAME "sec_touchscreen"
#define SOCK_PATH "/run/s24-mouse-mode.sock"
#define CLEANUP_CMD "/usr/local/bin/s24-mouse-mode"
#define PAD_NAME "S24 mouse mode touchpad"
#define PAD_VENDOR 0x5324
#define PAD_PRODUCT 0x4d4d

#define NSLOTS 10
#define MAXCLIENTS 4

/* The panel: 6.2", 1080 x 2340 at 416 ppi, 65.9 x 142.9 mm. The touch chip reports 0..4095 on
 * both axes, so its units are not square. The touchpad gets 1/40 mm units, square, so libinput
 * moves the pointer the same in both directions. */
#define TMAX 4095
#define UPM 40                       /* touchpad units per mm */
#define PANEL_W (659 * UPM / 10)     /* 2636 */
#define PANEL_H (1429 * UPM / 10)    /* 5716 */

#define EDGE_ZONE (35 * UPM / 10)    /* a touch that starts this close to the top or bottom edge... */
#define EDGE_GO (8 * UPM)            /* ...and moves this far in, mostly straight, is a gesture */
#define EDGE_SIDEWAYS (6 * UPM)      /* this far sideways first: the pointer, after all */
#define EDGE_WAIT_MS 700             /* or this long without deciding: the pointer */
#define TAP_MOVE (2 * UPM)           /* an edge touch let go after less movement than this... */
#define TAP_MS 300                   /* ...and sooner than this is a tap, sent on as one */
#define HOLD_MOVE (4 * UPM)          /* three fingers moving less than this... */
#define HOLD_MS 800                  /* ...for this long end mouse mode */
#define PANEL_MS 1000                /* the panel's state: read this long after the last touch, and
                                        this often after that, while on */

static int verbose;
static const char *dev_path_opt, *dpms_opt;
static char panel_state_path[256], panel_enabled_path[256];
static long long last_touch;

enum mode { M_OFF, M_ON, M_PAUSED };
static enum mode mode = M_OFF;
static int want_grab, grabbed;
static int tfd = -1, ufd = -1, lfd = -1;
static int rot;                       /* quarter turns of the user's view against the panel */

/* the touchscreen as it is now (its own units) */
struct slot { int id, x, y; };
static struct slot in[NSLOTS];
static int in_slot, dropping;

/* the touchpad as last sent */
struct oslot { int active, x, y; };
static struct oslot out[NSLOTS];
static int out_slot = -1, out_fingers, next_id = 1;

/* what the touches since the first finger landed are for */
enum seq { S_IDLE, S_OBSERVE, S_FORWARD, S_EDGE, S_SWALLOW };
static enum seq seq = S_IDLE;
static int edge_top, edge_slot, edge_x0, edge_y0;
static long long edge_t0;
static int hold_armed, hold_blocked, hold_x[NSLOTS], hold_y[NSLOTS], hold_id[NSLOTS];
static long long hold_t0, next_dpms;

struct client { int fd; uid_t uid; pid_t pid; char buf[256]; int len; };
static struct client clients[MAXCLIENTS];

/* who turned mouse mode on, to clean up after if it goes */
static int owner = -1;
static uid_t owner_uid;
static gid_t owner_gid;
static char *owner_envbuf, **owner_env;

static volatile sig_atomic_t quit_signal;

static void logmsg(const char *fmt, ...)
{
    char t[32];
    time_t now = time(NULL);
    strftime(t, sizeof t, "%F %T", localtime(&now));
    fprintf(stderr, "%s ", t);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define dbg(...) do { if (verbose) logmsg(__VA_ARGS__); } while (0)

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---- events to clients ------------------------------------------------------------------ */

static void broadcast(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    for (int i = 0; i < MAXCLIENTS; i++)
        if (clients[i].fd >= 0 && send(clients[i].fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
            dbg("client %d: %s", i, strerror(errno));
    dbg("-> %.*s", n - 1, line);
}

static const char *mode_name(void)
{
    return mode == M_ON ? "on" : mode == M_PAUSED ? "paused" : "off";
}

/* ---- the touchscreen -------------------------------------------------------------------- */

static int find_touchscreen(char *path, size_t len)
{
    if (dev_path_opt) {
        snprintf(path, len, "%s", dev_path_opt);
        return 0;
    }
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[300], name[128] = "";
        snprintf(p, sizeof p, "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (!strcmp(name, TOUCH_NAME)) {
            snprintf(path, len, "/dev/input/%s", e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* the whole multitouch state, after opening or after the kernel dropped events */
static void resync(void)
{
    struct { __u32 code; __s32 v[NSLOTS]; } m;
    static const int codes[3] = { ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
    for (int c = 0; c < 3; c++) {
        memset(&m, 0, sizeof m);
        m.code = codes[c];
        if (ioctl(tfd, EVIOCGMTSLOTS(sizeof m), &m) < 0) {
            logmsg("EVIOCGMTSLOTS: %s", strerror(errno));
            if (c == 0) for (int i = 0; i < NSLOTS; i++) in[i].id = -1;
            continue;
        }
        for (int i = 0; i < NSLOTS; i++) {
            if (c == 0) in[i].id = m.v[i];
            else if (c == 1) in[i].x = m.v[i];
            else in[i].y = m.v[i];
        }
    }
    struct input_absinfo ai;
    in_slot = ioctl(tfd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0 ? ai.value : 0;
}

static int open_touchscreen(void)
{
    if (tfd >= 0) return 0;
    char path[300];
    if (find_touchscreen(path, sizeof path) < 0) {
        logmsg("no %s", TOUCH_NAME);
        return -1;
    }
    tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (tfd < 0) {
        logmsg("%s: %s", path, strerror(errno));
        return -1;
    }
    resync();
    dropping = 0;
    logmsg("touchscreen %s", path);
    return 0;
}

static void close_touchscreen(void)
{
    if (tfd < 0) return;
    if (grabbed) ioctl(tfd, EVIOCGRAB, 0);
    grabbed = 0;
    close(tfd);
    tfd = -1;
}

static int fingers_down(void)
{
    int n = 0;
    for (int i = 0; i < NSLOTS; i++) n += in[i].id >= 0;
    return n;
}

/* touch chip units -> touchpad units in the user's view */
static void to_user(int x, int y, int *ux, int *uy)
{
    int px = (int)((long)x * PANEL_W / TMAX), py = (int)((long)y * PANEL_H / TMAX);
    switch (rot) {
    case 1: *ux = py; *uy = PANEL_W - px; break;
    case 2: *ux = PANEL_W - px; *uy = PANEL_H - py; break;
    case 3: *ux = PANEL_H - py; *uy = px; break;
    default: *ux = px; *uy = py; break;
    }
}
static int user_w(void) { return rot & 1 ? PANEL_H : PANEL_W; }
static int user_h(void) { return rot & 1 ? PANEL_W : PANEL_H; }

/* ---- the touchpad ----------------------------------------------------------------------- */

static struct input_event obuf[128];
static int on_count;

static void flush_pad(void)
{
    if (ufd >= 0 && on_count) {
        ssize_t want = on_count * (ssize_t)sizeof obuf[0];
        if (write(ufd, obuf, want) != want) logmsg("uinput write: %s", strerror(errno));
    }
    on_count = 0;
}

static void emit(int type, int code, int value)
{
    obuf[on_count].type = type;
    obuf[on_count].code = code;
    obuf[on_count].value = value;
    if (++on_count == (int)(sizeof obuf / sizeof obuf[0]) - 1) flush_pad();
}

static void abs_setup(int code, int max, int res)
{
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = code;
    a.absinfo.maximum = max;
    a.absinfo.resolution = res;
    ioctl(ufd, UI_SET_ABSBIT, code);
    if (ioctl(ufd, UI_ABS_SETUP, &a) < 0) logmsg("UI_ABS_SETUP %d: %s", code, strerror(errno));
}

static int make_pad(void)
{
    if (ufd >= 0) return 0;
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        logmsg("/dev/uinput: %s", strerror(errno));
        return -1;
    }
    static const int keys[] = { BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) ioctl(ufd, UI_SET_KEYBIT, keys[i]);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    abs_setup(ABS_X, user_w(), UPM);
    abs_setup(ABS_Y, user_h(), UPM);
    abs_setup(ABS_MT_SLOT, NSLOTS - 1, 0);
    abs_setup(ABS_MT_POSITION_X, user_w(), UPM);
    abs_setup(ABS_MT_POSITION_Y, user_h(), UPM);
    abs_setup(ABS_MT_TRACKING_ID, 65535, 0);
    ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = PAD_VENDOR;
    us.id.product = PAD_PRODUCT;
    us.id.version = 1;
    snprintf(us.name, sizeof us.name, "%s", PAD_NAME);
    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        logmsg("uinput create: %s", strerror(errno));
        close(ufd);
        ufd = -1;
        return -1;
    }
    memset(out, 0, sizeof out);
    out_slot = -1;
    out_fingers = 0;
    logmsg("touchpad up (rotation %d)", rot);
    return 0;
}

static void destroy_pad(void)
{
    if (ufd < 0) return;
    on_count = 0;
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    ufd = -1;
    logmsg("touchpad gone");
}

static void set_fingers(int n)
{
    static const int tools[] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                 BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
    int was = out_fingers > 5 ? 5 : out_fingers, now = n > 5 ? 5 : n;
    if (!!out_fingers != !!n) emit(EV_KEY, BTN_TOUCH, n > 0);
    if (was != now) {
        if (was) emit(EV_KEY, tools[was - 1], 0);
        if (now) emit(EV_KEY, tools[now - 1], 1);
    }
    out_fingers = n;
}

/* Bring the touchpad to what the touchscreen shows, or to no fingers when nothing goes on. */
static void sync_pad(void)
{
    if (ufd < 0) return;
    int fwd = seq == S_FORWARD && mode == M_ON && grabbed;
    int n = 0, firstx = -1, firsty = -1, changed = 0;
    for (int i = 0; i < NSLOTS; i++) {
        int active = fwd && in[i].id >= 0, x = 0, y = 0;
        if (active) to_user(in[i].x, in[i].y, &x, &y);
        if (active != out[i].active || (active && (x != out[i].x || y != out[i].y))) {
            if (out_slot != i) emit(EV_ABS, ABS_MT_SLOT, out_slot = i);
            if (active && !out[i].active) {
                emit(EV_ABS, ABS_MT_TRACKING_ID, next_id);
                next_id = next_id % 65535 + 1;
            } else if (!active) {
                emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
            }
            if (active && (x != out[i].x || !out[i].active)) emit(EV_ABS, ABS_MT_POSITION_X, x);
            if (active && (y != out[i].y || !out[i].active)) emit(EV_ABS, ABS_MT_POSITION_Y, y);
            out[i].active = active;
            out[i].x = x;
            out[i].y = y;
            changed = 1;
        }
        if (active) {
            if (firstx < 0) { firstx = x; firsty = y; }
            n++;
        }
    }
    if (n != out_fingers) {
        set_fingers(n);
        changed = 1;
    }
    if (changed) {
        if (n) {
            emit(EV_ABS, ABS_X, firstx);
            emit(EV_ABS, ABS_Y, firsty);
        }
        emit(EV_SYN, SYN_REPORT, 0);
        flush_pad();
    }
}

/* A tap where a touch at the edge was let go without becoming a gesture. */
static void tap_pad(void)
{
    if (ufd < 0 || out_fingers || mode != M_ON || !grabbed) return;
    int x = user_w() / 2, y = user_h() / 2;
    emit(EV_ABS, ABS_MT_SLOT, out_slot = 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, next_id);
    next_id = next_id % 65535 + 1;
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    set_fingers(1);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    emit(EV_SYN, SYN_REPORT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    set_fingers(0);
    emit(EV_SYN, SYN_REPORT, 0);
    flush_pad();
    dbg("tap at the edge");
}

/* ---- modes ------------------------------------------------------------------------------ */

static void spawn_cleanup(void)
{
    if (!owner_env) return;
    pid_t p = fork();
    if (p == 0) {
        struct passwd *pw = getpwuid(owner_uid);
        if (pw) initgroups(pw->pw_name, owner_gid);
        else setgroups(0, NULL);
        if (setgid(owner_gid) || setuid(owner_uid)) _exit(126);
        for (int fd = 3; fd < 256; fd++) close(fd);
        signal(SIGCHLD, SIG_DFL);   /* not ours to pass on: the cleanup waits for its children */
        signal(SIGPIPE, SIG_DFL);
        char *argv[] = { CLEANUP_CMD, "cleanup", NULL };
        execve(CLEANUP_CMD, argv, owner_env);
        _exit(127);
    }
    logmsg(p > 0 ? "cleanup started as uid %d" : "cleanup: fork failed (uid %d)", (int)owner_uid);
}

static void forget_owner(void)
{
    owner = -1;
    free(owner_envbuf);
    free(owner_env);
    owner_envbuf = NULL;
    owner_env = NULL;
}

/* Take or let go of the touchscreen once no finger is down; finish turning off. */
static void settle(void)
{
    if (tfd < 0) return;
    if (fingers_down()) return;
    if (want_grab && !grabbed) {
        if (ioctl(tfd, EVIOCGRAB, 1) == 0) {
            grabbed = 1;
            logmsg("touchscreen taken");
        } else {
            logmsg("EVIOCGRAB: %s", strerror(errno));
        }
    } else if (!want_grab && grabbed) {
        ioctl(tfd, EVIOCGRAB, 0);
        grabbed = 0;
        logmsg("touchscreen let go");
    }
    if (mode == M_OFF) {
        destroy_pad();
        close_touchscreen();
    }
}

static void set_mode(enum mode m)
{
    if (m == mode) return;
    mode = m;
    want_grab = m == M_ON;
    sync_pad();
    settle();
    broadcast("state %s", mode_name());
}

/* Mouse mode ends without being asked to. at_once: now, fingers or not (the panel went dark:
 * the touch chip may never report the fingers going, and s24-powerd needs the device). */
static void end_mode(const char *why, int at_once)
{
    if (mode == M_OFF) return;
    logmsg("mouse mode ends: %s", why);
    if (at_once) {
        seq = S_SWALLOW;
        sync_pad();
        for (int i = 0; i < NSLOTS; i++) in[i].id = -1;
    }
    seq = fingers_down() ? S_SWALLOW : S_IDLE;
    broadcast("exit %s", why);
    set_mode(M_OFF);
}

static ssize_t read_file(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    ssize_t r;
    while (got < len - 1 && (r = read(fd, buf + got, len - 1 - got)) > 0) got += r;
    close(fd);
    buf[got] = 0;
    return got;
}

/* 1 dark, 0 lit or unknown (unknown never ends mouse mode) */
static int panel_dark(void)
{
    static char dump[65536];
    char s[32];
    if (panel_enabled_path[0] && read_file(panel_enabled_path, s, sizeof s) > 0 && !strncmp(s, "disabled", 8))
        return 1;
    if (!panel_state_path[0] || read_file(panel_state_path, dump, sizeof dump) <= 0) return 0;
    if (!strstr(dump, "connector[")) return !strncmp(dump, "Off", 3);   /* a test's stand-in file */
    int inside = 0;
    for (char *line = dump, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (*line != ' ' && *line != '\t') {
            size_t l = strlen(line);
            inside = !strncmp(line, "connector[", 10) && l > 7 && !strcmp(line + l - 7, ": DSI-1");
            continue;
        }
        if (!inside) continue;
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "state:", 6)) {
            line += 6;
            while (*line == ' ') line++;
            return !strncmp(line, "off", 3);
        }
    }
    return 0;
}

/* ---- gestures --------------------------------------------------------------------------- */

static void start_sequence(long long t)
{
    hold_armed = hold_blocked = 0;
    if (!(mode == M_ON && grabbed)) {
        seq = S_OBSERVE;
        return;
    }
    int n = 0, s = -1;
    for (int i = 0; i < NSLOTS; i++)
        if (in[i].id >= 0) { n++; s = i; }
    if (n == 1) {
        int x, y;
        to_user(in[s].x, in[s].y, &x, &y);
        if (y < EDGE_ZONE || y > user_h() - EDGE_ZONE) {
            seq = S_EDGE;
            edge_top = y < EDGE_ZONE;
            edge_slot = s;
            edge_x0 = x;
            edge_y0 = y;
            edge_t0 = t;
            dbg("edge touch (%s)", edge_top ? "top" : "bottom");
            return;
        }
    }
    seq = S_FORWARD;
}

static void edge_update(long long t, int n)
{
    if (n > 1) {
        seq = S_FORWARD;     /* a second finger: two-finger scroll or tap, not a gesture */
        return;
    }
    if (n == 0 || in[edge_slot].id < 0) {
        /* let go before deciding (n == 0 is handled by the caller) */
        return;
    }
    int x, y;
    to_user(in[edge_slot].x, in[edge_slot].y, &x, &y);
    int inward = edge_top ? y - edge_y0 : edge_y0 - y, sideways = abs(x - edge_x0);
    if (inward >= EDGE_GO && sideways < inward) {
        seq = S_SWALLOW;
        broadcast("edge %s", edge_top ? "top" : "bottom");
    } else if (sideways >= EDGE_SIDEWAYS || inward <= -TAP_MOVE || t - edge_t0 >= EDGE_WAIT_MS) {
        seq = S_FORWARD;
        dbg("edge touch is the pointer");
    }
}

static void hold_update(long long t, int n)
{
    if (n != 3) {
        hold_armed = hold_blocked = 0;
        return;
    }
    if (hold_blocked) return;
    if (!hold_armed) {
        hold_armed = 1;
        hold_t0 = t;
        for (int i = 0; i < NSLOTS; i++) {
            hold_id[i] = in[i].id;
            to_user(in[i].x, in[i].y, &hold_x[i], &hold_y[i]);
        }
        return;
    }
    for (int i = 0; i < NSLOTS; i++) {
        if (in[i].id != hold_id[i]) { hold_armed = 0; hold_blocked = 1; return; }
        if (in[i].id < 0) continue;
        int x, y;
        to_user(in[i].x, in[i].y, &x, &y);
        if (abs(x - hold_x[i]) > HOLD_MOVE || abs(y - hold_y[i]) > HOLD_MOVE) {
            hold_armed = 0;
            hold_blocked = 1;   /* a pinch or a swipe, not a hold, until the fingers change */
            return;
        }
    }
}

/* timers: an edge touch that waited long enough, three fingers held long enough */
static void timers(long long t)
{
    if (seq == S_EDGE && t - edge_t0 >= EDGE_WAIT_MS) {
        seq = S_FORWARD;
        dbg("edge touch is the pointer (time)");
        sync_pad();
    }
    if (seq == S_FORWARD && hold_armed && t - hold_t0 >= HOLD_MS) {
        hold_armed = 0;
        seq = S_SWALLOW;
        sync_pad();         /* the touchpad sees the three fingers go */
        end_mode("hold", 0);
    }
}

/* After every report from the touchscreen (and after a resync). */
static void frame(void)
{
    long long t = now_ms();
    int n = fingers_down();
    if (seq == S_IDLE && n > 0) start_sequence(t);
    if (seq == S_EDGE) {
        if (n == 0) {
            /* let go before it was decided: a tap if it barely moved and was quick (the
             * position of a lifted slot is its last one) */
            int x, y;
            to_user(in[edge_slot].x, in[edge_slot].y, &x, &y);
            if (abs(x - edge_x0) + abs(y - edge_y0) < TAP_MOVE && t - edge_t0 < TAP_MS) tap_pad();
        } else {
            edge_update(t, n);
        }
    }
    if (seq == S_FORWARD) hold_update(t, n);
    if (n == 0) seq = S_IDLE;
    sync_pad();
    timers(t);
    if (n == 0) settle();
}

static void read_touchscreen(void)
{
    struct input_event ev[64];
    for (;;) {
        ssize_t r = read(tfd, ev, sizeof ev);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            logmsg("touchscreen read: %s", strerror(errno));
            end_mode("error", 1);
            close_touchscreen();
            return;
        }
        if (r == 0) return;
        last_touch = now_ms();
        for (size_t i = 0; i < r / sizeof ev[0]; i++) {
            struct input_event *e = &ev[i];
            if (dropping) {
                if (e->type == EV_SYN && e->code == SYN_REPORT) {
                    dropping = 0;
                    resync();
                    frame();
                }
                continue;
            }
            if (e->type == EV_SYN) {
                if (e->code == SYN_DROPPED) {
                    dropping = 1;
                    dbg("events dropped");
                } else if (e->code == SYN_REPORT) {
                    frame();
                    if (tfd < 0) return;
                }
            } else if (e->type == EV_ABS) {
                if (e->code == ABS_MT_SLOT) {
                    in_slot = e->value;
                } else if (in_slot >= 0 && in_slot < NSLOTS) {
                    if (e->code == ABS_MT_TRACKING_ID) in[in_slot].id = e->value;
                    else if (e->code == ABS_MT_POSITION_X) in[in_slot].x = e->value;
                    else if (e->code == ABS_MT_POSITION_Y) in[in_slot].y = e->value;
                }
            }
            /* keys (BTN_TOUCH, BTN_TOOL_FINGER, Samsung's palm keys, KEY_WAKEUP) are the
             * touchpad's own business or not ours */
        }
    }
}

/* ---- control ---------------------------------------------------------------------------- */

static uid_t session_uid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return (uid_t)-1;
    struct dirent *e;
    uid_t uid = (uid_t)-1;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], comm[32] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, comm, sizeof comm - 1);
        close(fd);
        if (n > 0 && !strncmp(comm, "kwin_wayland\n", 13)) {
            struct stat st;
            snprintf(p, sizeof p, "/proc/%s", e->d_name);
            if (stat(p, &st) == 0) uid = st.st_uid;
            break;
        }
    }
    closedir(d);
    return uid;
}

static void keep_owner_env(int c)
{
    forget_owner();
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/environ", (int)clients[c].pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    ssize_t r;
    while (buf && (r = read(fd, buf + len, cap - len)) > 0) len += r;
    close(fd);
    if (!buf || !len) { free(buf); return; }
    buf[len] = 0;
    size_t count = 0;
    for (size_t i = 0; i < len; i++) count += buf[i] == 0;
    char **env = calloc(count + 2, sizeof *env);
    if (!env) { free(buf); return; }
    size_t k = 0;
    for (size_t i = 0; i < len; i += strlen(buf + i) + 1)
        if (buf[i] && strncmp(buf + i, "LD_PRELOAD=", 11)) env[k++] = buf + i;
    env[k] = NULL;
    struct stat st;
    snprintf(p, sizeof p, "/proc/%d", (int)clients[c].pid);
    owner = c;
    owner_uid = clients[c].uid;
    owner_gid = stat(p, &st) == 0 ? st.st_gid : clients[c].uid;
    owner_envbuf = buf;
    owner_env = env;
}

static void command(int c, char *line)
{
    dbg("<- %s", line);
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "on") || !strcmp(line, "resume")) {
        if (mode == M_OFF) {
            if (!strcmp(line, "resume")) { broadcast("state off"); return; }
            if (panel_dark()) { broadcast("exit dark"); broadcast("state off"); return; }
            if (open_touchscreen() < 0 || make_pad() < 0) {
                destroy_pad();
                close_touchscreen();
                broadcast("exit error");
                broadcast("state off");
                return;
            }
            keep_owner_env(c);
            next_dpms = now_ms() + PANEL_MS;
            seq = fingers_down() ? S_OBSERVE : S_IDLE;
        }
        set_mode(M_ON);
    } else if (!strcmp(line, "pause")) {
        if (mode == M_ON) set_mode(M_PAUSED);
        else broadcast("state %s", mode_name());
    } else if (!strcmp(line, "off")) {
        forget_owner();
        if (mode == M_OFF) broadcast("state off");
        else set_mode(M_OFF);
    } else if (!strcmp(line, "rotate") && arg) {
        int r = atoi(arg) & 3;
        if (r != rot) {
            rot = r;
            if (ufd >= 0) {
                /* a touchpad's size is fixed when it is made: a new one, the other way round;
                 * fingers down now are not sent on until they go */
                seq = S_SWALLOW;
                sync_pad();
                destroy_pad();
                make_pad();
                if (!fingers_down()) seq = S_IDLE;
            }
            logmsg("rotation %d", rot);
        }
    } else if (!strcmp(line, "status")) {
        broadcast("state %s", mode_name());
    } else {
        dbg("unknown command %s", line);
    }
}

static void drop_client(int c)
{
    close(clients[c].fd);
    clients[c].fd = -1;
    clients[c].len = 0;
    if (c == owner) {
        int was_on = mode != M_OFF;
        if (was_on) {
            logmsg("the client that turned mouse mode on went away");
            seq = fingers_down() ? S_SWALLOW : S_IDLE;
            set_mode(M_OFF);
            spawn_cleanup();
        }
        forget_owner();
    }
}

static void accept_client(void)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    struct ucred cr;
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0 ||
        (cr.uid != 0 && cr.uid != session_uid())) {
        logmsg("refused a client (uid %d)", (int)cr.uid);
        close(fd);
        return;
    }
    for (int i = 0; i < MAXCLIENTS; i++) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            clients[i].uid = cr.uid;
            clients[i].pid = cr.pid;
            clients[i].len = 0;
            dbg("client %d: uid %d pid %d", i, (int)cr.uid, (int)cr.pid);
            return;
        }
    }
    close(fd);
}

static void read_client(int c)
{
    struct client *cl = &clients[c];
    ssize_t r = recv(cl->fd, cl->buf + cl->len, sizeof cl->buf - 1 - cl->len, 0);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
        drop_client(c);
        return;
    }
    cl->len += r;
    cl->buf[cl->len] = 0;
    char *nl;
    while ((nl = memchr(cl->buf, '\n', cl->len))) {
        *nl = 0;
        char line[256];
        snprintf(line, sizeof line, "%s", cl->buf);
        size_t used = nl - cl->buf + 1;
        memmove(cl->buf, nl + 1, cl->len - used);
        cl->len -= used;
        if (line[0]) command(c, line);
        if (cl->fd < 0) return;
    }
    if (cl->len >= (int)sizeof cl->buf - 1) cl->len = 0;   /* a line too long: dropped */
}

static void on_signal(int sig) { quit_signal = sig; }

static void find_panel(void)
{
    if (dpms_opt) {
        if (strcmp(dpms_opt, "none")) snprintf(panel_state_path, sizeof panel_state_path, "%s", dpms_opt);
        return;
    }
    DIR *d = opendir("/sys/kernel/debug/dri");
    struct dirent *e;
    char p[300], name[128];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/kernel/debug/dri/%s/name", e->d_name);
        if (read_file(p, name, sizeof name) > 0 && strstr(name, "exynos")) {
            snprintf(panel_state_path, sizeof panel_state_path, "/sys/kernel/debug/dri/%s/state", e->d_name);
            break;
        }
    }
    if (d) closedir(d);
    d = opendir("/sys/class/drm");
    while (d && (e = readdir(d)))
        if (strstr(e->d_name, "-DSI-1")) {
            snprintf(panel_enabled_path, sizeof panel_enabled_path, "/sys/class/drm/%s/enabled", e->d_name);
            break;
        }
    if (d) closedir(d);
}

int main(int argc, char **argv)
{
    const char *sock_path = SOCK_PATH;
    int panel_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--panel")) panel_only = 1;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) dev_path_opt = argv[++i];
        else if (!strcmp(argv[i], "--dpms") && i + 1 < argc) dpms_opt = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock_path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-v] [--device PATH] [--dpms none|PATH] [--socket PATH] | --panel\n", argv[0]);
            return 2;
        }
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    for (int i = 0; i < MAXCLIENTS; i++) clients[i].fd = -1;
    for (int i = 0; i < NSLOTS; i++) in[i].id = -1;
    find_panel();
    if (panel_only) {   /* what it would see now, and where it looks */
        printf("%s (%s, %s)\n", panel_dark() ? "dark" : "lit", panel_state_path[0] ? panel_state_path : "-",
               panel_enabled_path[0] ? panel_enabled_path : "-");
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    unlink(sock_path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 4) < 0) {
        logmsg("%s: %s", sock_path, strerror(errno));
        return 1;
    }
    chmod(sock_path, 0666);   /* who may talk is checked per connection (SO_PEERCRED) */
    logmsg("ready on %s (panel state from %s, %s)", sock_path,
           panel_state_path[0] ? panel_state_path : "nowhere", panel_enabled_path[0] ? panel_enabled_path : "-");

    while (!quit_signal) {
        struct pollfd pf[2 + MAXCLIENTS];
        int np = 0, ti = -1, ci[MAXCLIENTS];
        pf[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        if (tfd >= 0) { ti = np; pf[np++] = (struct pollfd){ .fd = tfd, .events = POLLIN }; }
        for (int i = 0; i < MAXCLIENTS; i++) {
            ci[i] = -1;
            if (clients[i].fd >= 0) { ci[i] = np; pf[np++] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN }; }
        }
        long long t = now_ms(), wake = -1;
        if (mode != M_OFF) wake = next_dpms;
        if (seq == S_EDGE && (wake < 0 || edge_t0 + EDGE_WAIT_MS < wake)) wake = edge_t0 + EDGE_WAIT_MS;
        if (seq == S_FORWARD && hold_armed && (wake < 0 || hold_t0 + HOLD_MS < wake)) wake = hold_t0 + HOLD_MS;
        int timeout = wake < 0 ? -1 : wake <= t ? 0 : (int)(wake - t);
        int r = poll(pf, np, timeout);
        if (r < 0 && errno != EINTR) {
            logmsg("poll: %s", strerror(errno));
            break;
        }
        t = now_ms();
        if (r > 0) {
            if (pf[0].revents & POLLIN) accept_client();
            if (ti >= 0 && tfd >= 0 && (pf[ti].revents & (POLLIN | POLLERR | POLLHUP))) read_touchscreen();
            for (int i = 0; i < MAXCLIENTS; i++)
                if (ci[i] >= 0 && clients[i].fd >= 0 && (pf[ci[i]].revents & (POLLIN | POLLERR | POLLHUP)))
                    read_client(i);
        }
        timers(t);
        if (mode != M_OFF && t >= next_dpms) {
            /* while touches come the panel is lit; once they stop, look */
            if (t - last_touch < PANEL_MS) {
                next_dpms = last_touch + PANEL_MS;
            } else {
                next_dpms = t + PANEL_MS;
                if (panel_dark()) end_mode("dark", 1);
            }
        }
    }
    logmsg("stopping (signal %d)", (int)quit_signal);
    if (mode != M_OFF) {
        int had_owner = owner >= 0;
        seq = S_SWALLOW;
        sync_pad();
        if (grabbed) { ioctl(tfd, EVIOCGRAB, 0); grabbed = 0; }
        mode = M_OFF;
        broadcast("exit error");
        if (had_owner) spawn_cleanup();
    }
    destroy_pad();
    close_touchscreen();
    unlink(sock_path);
    return 0;
}
