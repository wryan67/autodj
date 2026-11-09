#include "keys.h"

#include "config.h"
#include "log.h"
#include "player.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *name;
    int code;
} g_keys[] = {
#define X(name) {#name, name},
#include "keytab.inc"
#undef X
};

/* A binding is a key code with the held modifiers in bits 16 and up. */
enum {
    MOD_CTRL = 1,
    MOD_SHIFT = 2,
    MOD_ALT = 4,
    MOD_META = 8,
    MOD_SHIFT_BITS = 16,
    KEY_CODE_MASK = 0xffff
};

static const struct {
    const char *name;
    int bit;
} g_mods[] = {
    {"CTRL", MOD_CTRL}, {"SHIFT", MOD_SHIFT}, {"ALT", MOD_ALT}, {"META", MOD_META},
};

/* Modifier state for one device. lone is a modifier pressed with nothing
 * else held, until another key goes down. */
struct Combo {
    int mods;
    int lone;
};

enum { MAX_PADS = 8, MAX_CAPTURE_FD = 256 };

static int g_fds[MAX_PADS];
static struct Combo g_combo[MAX_PADS];
static int g_nfd;
static int g_ngrabbed;
static void (*g_jog_hook)(int direction, long long when_us);
static struct Combo g_cap[MAX_CAPTURE_FD];
static struct timespec g_vol_time;
static struct timeval g_press_time;
static int g_press_seen;

enum { KEY_DEBOUNCE_MS = 30 };

static int mod_bit(int code)
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return MOD_CTRL;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return MOD_SHIFT;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        return MOD_ALT;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return MOD_META;
    default:
        return 0;
    }
}

static const char *base_name(int code)
{
    size_t i;

    for (i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        if (g_keys[i].code == code) {
            return g_keys[i].name;
        }
    }
    return NULL;
}

int key_from_name(const char *name, int *code)
{
    int mods = 0;
    size_t i;
    const char *plus;

    while ((plus = strchr(name, '+')) != NULL) {
        size_t len = (size_t)(plus - name);
        int bit = 0;

        for (i = 0; i < sizeof g_mods / sizeof g_mods[0]; i++) {
            if (strlen(g_mods[i].name) == len && strncasecmp(g_mods[i].name, name, len) == 0) {
                bit = g_mods[i].bit;
            }
        }
        if (!bit || (mods & bit)) {
            return -1;
        }
        mods |= bit;
        name = plus + 1;
    }
    for (i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        if (strcmp(g_keys[i].name, name) == 0) {
            if (mods && mod_bit(g_keys[i].code)) {
                return -1;
            }
            *code = g_keys[i].code | (mods << MOD_SHIFT_BITS);
            return 0;
        }
    }
    return -1;
}

const char *key_name(int code)
{
    static char buf[4][64];
    static int slot;
    const char *base;
    char *out;
    int mods;
    size_t i;

    if (code < 0) {
        return NULL;
    }
    base = base_name(code & KEY_CODE_MASK);
    mods = code >> MOD_SHIFT_BITS;
    if (!base || !mods) {
        return base;
    }
    out = buf[slot];
    slot = (slot + 1) % 4;
    out[0] = '\0';
    for (i = 0; i < sizeof g_mods / sizeof g_mods[0]; i++) {
        if (mods & g_mods[i].bit) {
            strcat(out, g_mods[i].name);
            strcat(out, "+");
        }
    }
    strncat(out, base, sizeof buf[0] - strlen(out) - 1);
    return out;
}

static void combo_reset(struct Combo *c)
{
    c->mods = 0;
    c->lone = -1;
}

/* Turn one event into a binding. Returns the binding with *value set to 1
 * for a press or 2 for a repeat, or -1. A modifier counts as its own press
 * when it is released without another key going down while it was held. */
static int combo_feed(struct Combo *c, const struct input_event *ev, int *value)
{
    int bit;

    if (ev->type != EV_KEY) {
        return -1;
    }
    bit = mod_bit(ev->code);
    if (bit) {
        if (ev->value == 1) {
            c->lone = c->mods ? -1 : (int)ev->code;
            c->mods |= bit;
        } else if (ev->value == 0) {
            c->mods &= ~bit;
            if (c->lone == (int)ev->code) {
                c->lone = -1;
                *value = 1;
                return (int)ev->code;
            }
            c->lone = -1;
        }
        return -1;
    }
    if (ev->value != 1 && ev->value != 2) {
        return -1;
    }
    c->lone = -1;
    *value = ev->value;
    return (int)ev->code | (c->mods << MOD_SHIFT_BITS);
}

static int bit_set(const unsigned long *bits, int code)
{
    size_t word = (size_t)code / (8 * sizeof(unsigned long));
    unsigned shift = (unsigned)code % (8 * sizeof(unsigned long));
    return (bits[word] & (1UL << shift)) != 0;
}

int keys_grab(int fd)
{
    if (fd < 0 || ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        return -1;
    }
    return 0;
}

void keys_ungrab(int fd)
{
    if (fd < 0) {
        return;
    }
    ioctl(fd, EVIOCGRAB, (void *)0);
}

static void drain_fd(int fd)
{
    struct input_event ev;

    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
    }
}

int keys_read_press(int fd)
{
    struct input_event ev;
    struct Combo spare;
    struct Combo *c;
    int found = -1;

    if (fd < 0) {
        return -1;
    }
    combo_reset(&spare);
    c = fd < MAX_CAPTURE_FD ? &g_cap[fd] : &spare;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof ev);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n != (ssize_t)sizeof ev) {
            break;
        }
        {
            int value = 0;
            int key = combo_feed(c, &ev, &value);
            int base = key & KEY_CODE_MASK;

            if (found >= 0 || key < 0 || value != 1) {
                continue;
            }
            if (base == KEY_NUMLOCK || base == KEY_RESERVED) {
                continue;
            }
            found = key;
        }
    }
    return found;
}

static int open_event(int index, char *name, size_t name_n, unsigned long *bits, size_t bits_n)
{
    char path[64];
    int fd;

    snprintf(path, sizeof path, "/dev/input/event%d", index);
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    memset(name, 0, name_n);
    memset(bits, 0, bits_n);
    if (ioctl(fd, EVIOCGNAME(name_n), name) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, bits_n), bits) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The USB keypad, or the keyboard interface of the XP-Pen ACK05 Shortcut
 * Remote. The remote's mouse and pen interfaces have no letter keys. */
static int pad_device(int fd, const char *name, const unsigned long *bits)
{
    struct input_id id;

    if (strcmp(name, "USB Keypad") == 0 && bit_set(bits, KEY_KP0)) {
        return 1;
    }
    memset(&id, 0, sizeof id);
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        return 0;
    }
    return id.vendor == 0x28bd && id.product == 0x0202 && bit_set(bits, KEY_Z);
}

int keys_open_capture(int *fds, int max, int *keypad)
{
    int pads[8];
    int others[32];
    int npads = 0;
    int nothers = 0;
    int n = 0;
    int i;

    if (keypad) {
        *keypad = 0;
    }
    if (!fds || max <= 0) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        char name[256];
        unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
        int fd;
        int pad;
        int board;

        fd = open_event(i, name, sizeof name, bits, sizeof bits);
        if (fd < 0) {
            continue;
        }
        /* The keypad also exposes mouse and consumer interfaces under the
         * same name. Those do not report KEY_KP0. A keyboard reports letters
         * or keypad digits, which leaves out mice, HDMI, and media controls. */
        pad = pad_device(fd, name, bits);
        board = bit_set(bits, KEY_A) || bit_set(bits, KEY_KP0);
        if (!pad && !board) {
            close(fd);
            continue;
        }
        drain_fd(fd);
        if (fd < MAX_CAPTURE_FD) {
            combo_reset(&g_cap[fd]);
        }
        if (pad) {
            if (npads < (int)(sizeof pads / sizeof pads[0])) {
                pads[npads++] = fd;
            } else {
                close(fd);
            }
        } else if (nothers < (int)(sizeof others / sizeof others[0])) {
            others[nothers++] = fd;
        } else {
            close(fd);
        }
    }
    if (keypad) {
        *keypad = npads < max ? npads : max;
    }
    for (i = 0; i < npads; i++) {
        if (n < max) {
            fds[n++] = pads[i];
        } else {
            close(pads[i]);
        }
    }
    for (i = 0; i < nothers; i++) {
        if (n < max) {
            fds[n++] = others[i];
        } else {
            close(others[i]);
        }
    }
    return n;
}

void keys_close_capture(int *fds, int n)
{
    int i;

    if (!fds || n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

int keys_open(void)
{
    int i;

    g_nfd = 0;
    g_ngrabbed = 0;
    for (i = 0; i < 32 && g_nfd < MAX_PADS; i++) {
        char name[256];
        unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
        int fd;

        fd = open_event(i, name, sizeof name, bits, sizeof bits);
        if (fd < 0) {
            continue;
        }
        if (!pad_device(fd, name, bits)) {
            close(fd);
            continue;
        }
        /* EVIOCGRAB takes the integer in the argument itself, not a pointer.
         * A non-zero value grabs; zero releases. */
        if (keys_grab(fd) < 0) {
            log_warn("keys", "could not grab %s (event%d): %s", name, i, strerror(errno));
        } else {
            log_info("keys", "%s (event%d) grabbed", name, i);
            g_ngrabbed++;
        }
        drain_fd(fd);
        combo_reset(&g_combo[g_nfd]);
        g_fds[g_nfd++] = fd;
    }
    if (g_nfd == 0) {
        log_warn("keys", "no USB keypad or shortcut remote found, playback will continue without keys");
        return -1;
    }
    return 0;
}

void keys_close(void)
{
    int i;

    for (i = 0; i < g_nfd; i++) {
        ioctl(g_fds[i], EVIOCGRAB, (void *)0);
        close(g_fds[i]);
        g_fds[i] = -1;
    }
    g_nfd = 0;
    g_ngrabbed = 0;
}

int keys_grabbed(void)
{
    return g_ngrabbed;
}

void keys_set_jog_hook(void (*fn)(int direction, long long when_us))
{
    g_jog_hook = fn;
}

int keys_fds(int *fds, int max)
{
    int i;

    for (i = 0; i < g_nfd && i < max; i++) {
        fds[i] = g_fds[i];
    }
    return i;
}

static int volume_due(void)
{
    struct timespec now;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - g_vol_time.tv_sec) * 1000 + (now.tv_nsec - g_vol_time.tv_nsec) / 1000000;
    if (g_vol_time.tv_sec == 0 && g_vol_time.tv_nsec == 0) {
        ms = 1000;
    }
    if (ms < 80) {
        return 0;
    }
    g_vol_time = now;
    return 1;
}

/* Milliseconds from earlier to later. Negative when later comes first. */
static long timeval_delta_ms(const struct timeval *later, const struct timeval *earlier)
{
    long sec = (long)later->tv_sec - (long)earlier->tv_sec;
    long usec = (long)later->tv_usec - (long)earlier->tv_usec;

    if (usec < 0) {
        sec -= 1;
        usec += 1000000;
    }
    return sec * 1000 + usec / 1000;
}

/* Presses closer than 30 ms are ignored. The gap is measured from the
 * previous press, including one that was itself ignored. */
static int press_accepted(const struct timeval *when)
{
    long ms = 1000;

    if (g_press_seen) {
        ms = timeval_delta_ms(when, &g_press_time);
    }
    g_press_time = *when;
    g_press_seen = 1;
    return ms >= KEY_DEBOUNCE_MS;
}

static void dispatch(const struct Config *cfg, int key, int value, const struct timeval *when)
{
    const char *name;

    if ((key & KEY_CODE_MASK) == KEY_NUMLOCK) {
        return;
    }
    /* Dial clicks come faster than the press gap when it spins, and the
     * player measures their spacing, so they skip the debounce. */
    if (key == cfg->key_jog_fwd || key == cfg->key_jog_back) {
        if (value == 1) {
            int dir = key == cfg->key_jog_fwd ? 1 : -1;
            long long us = (long long)when->tv_sec * 1000000LL + when->tv_usec;

            player_jog(dir, us);
            if (g_jog_hook) {
                g_jog_hook(dir, us);
            }
        }
        return;
    }
    /* Value 1 is a press. Repeats stay on the volume hold path. */
    if (value == 1 && !press_accepted(when)) {
        return;
    }
    if (key == cfg->key_pause && value == 1) {
        player_toggle_pause();
        return;
    }
    if (key == cfg->key_skip && value == 1) {
        player_skip();
        return;
    }
    if (key == cfg->key_prev && value == 1) {
        player_previous();
        return;
    }
    if (key == cfg->key_rew05 && value == 1) {
        player_rewind(500);
        return;
    }
    if (key == cfg->key_rew10 && value == 1) {
        player_rewind(1000);
        return;
    }
    if (key == cfg->key_rew15 && value == 1) {
        player_rewind(1500);
        return;
    }
    if (key == cfg->key_vol_up || key == cfg->key_vol_dn) {
        if (volume_due()) {
            player_volume_step(key == cfg->key_vol_up ? 1 : -1);
        }
        return;
    }
    if (value == 1) {
        name = key_name(key);
        log_info("keys", "key %s is not mapped", name ? name : "unknown");
    }
}

void keys_poll(const struct Config *cfg)
{
    struct input_event ev;
    int i;

    if (!cfg) {
        return;
    }
    for (i = 0; i < g_nfd; i++) {
        while (read(g_fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            int value = 0;
            int key = combo_feed(&g_combo[i], &ev, &value);

            if (key >= 0) {
                dispatch(cfg, key, value, &ev.time);
            }
        }
    }
}

static int put_key(int fd, long ms, unsigned short code, int value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.time.tv_sec = ms / 1000;
    ev.time.tv_usec = (ms % 1000) * 1000;
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) {
        return -1;
    }
    return 0;
}

static int debounce_self_test(void)
{
    int pipefd[2];
    Config cfg;
    int rc = -1;

    if (pipe(pipefd) != 0) {
        log_error("keys", "cannot create a debounce pipe");
        return -1;
    }
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) != 0) {
        log_error("keys", "cannot set the debounce pipe nonblocking");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.key_pause = KEY_A;
    cfg.key_skip = KEY_B;
    cfg.key_prev = -1;
    cfg.key_rew05 = -1;
    cfg.key_rew10 = -1;
    cfg.key_rew15 = -1;
    cfg.key_vol_up = KEY_KPPLUS;
    cfg.key_vol_dn = KEY_KPMINUS;
    cfg.key_jog_fwd = -1;
    cfg.key_jog_back = -1;
    player_set_volume_percent(50);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    memset(&g_vol_time, 0, sizeof g_vol_time);
    g_fds[0] = pipefd[0];
    combo_reset(&g_combo[0]);
    g_nfd = 1;

    /* NumLock is not a press, so the keypad key beside it still counts. */
    if (put_key(pipefd[1], 10000, KEY_NUMLOCK, 1) != 0 ||
        put_key(pipefd[1], 10010, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write debounce events");
        goto out;
    }
    keys_poll(&cfg);
    if (player_volume_percent() != 55) {
        log_error("keys", "first press left volume %d, expected 55",
                  player_volume_percent());
        goto out;
    }

    /* 20 ms later is inside the gap. Clear the hold timer so only the
     * press gap can reject this event. */
    memset(&g_vol_time, 0, sizeof g_vol_time);
    if (put_key(pipefd[1], 10030, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a close press");
        goto out;
    }
    keys_poll(&cfg);
    if (player_volume_percent() != 55) {
        log_error("keys", "close press left volume %d, expected 55",
                  player_volume_percent());
        goto out;
    }

    /* Another key starts the gap again. */
    if (put_key(pipefd[1], 10070, KEY_A, 1) != 0 ||
        put_key(pipefd[1], 10090, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a pause press");
        goto out;
    }
    memset(&g_vol_time, 0, sizeof g_vol_time);
    keys_poll(&cfg);
    if (player_volume_percent() != 55) {
        log_error("keys", "press after pause left volume %d, expected 55",
                  player_volume_percent());
        goto out;
    }

    /* 30 ms after the previous press is accepted. */
    memset(&g_vol_time, 0, sizeof g_vol_time);
    if (put_key(pipefd[1], 10120, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a later press");
        goto out;
    }
    keys_poll(&cfg);
    if (player_volume_percent() != 60) {
        log_error("keys", "press at 30 ms left volume %d, expected 60",
                  player_volume_percent());
        goto out;
    }

    /* A repeat inside the press gap still changes volume. A press does not. */
    memset(&g_vol_time, 0, sizeof g_vol_time);
    if (put_key(pipefd[1], 10140, KEY_KPPLUS, 2) != 0 ||
        put_key(pipefd[1], 10145, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a volume repeat");
        goto out;
    }
    keys_poll(&cfg);
    if (player_volume_percent() != 65) {
        log_error("keys", "repeat during debounce left volume %d, expected 65",
                  player_volume_percent());
        goto out;
    }
    rc = 0;
out:
    g_fds[0] = -1;
    g_nfd = 0;
    close(pipefd[0]);
    close(pipefd[1]);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    memset(&g_vol_time, 0, sizeof g_vol_time);
    player_set_volume_percent(100);
    return rc;
}

static int feed(struct Combo *c, int code, int value, int *out_value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = (unsigned short)code;
    ev.value = value;
    return combo_feed(c, &ev, out_value);
}

/* The shortcut remote sends Ctrl+Z, Ctrl+Shift+Z, and a bare Ctrl on
 * separate buttons. Each one has to come out as its own binding. */
static int combo_self_test(void)
{
    struct Combo c;
    int v = 0;
    int code = -1;
    int ctrl_z = -1;
    int ctrl_shift_z = -1;

    if (key_from_name("CTRL+KEY_Z", &ctrl_z) != 0 ||
        key_from_name("ctrl+shift+KEY_Z", &ctrl_shift_z) != 0 ||
        ctrl_z == ctrl_shift_z || ctrl_z == KEY_Z) {
        log_error("keys", "combo name lookup failed");
        return -1;
    }
    if (!key_name(ctrl_shift_z) || strcmp(key_name(ctrl_shift_z), "CTRL+SHIFT+KEY_Z") != 0) {
        log_error("keys", "combo name is %s", key_name(ctrl_shift_z) ? key_name(ctrl_shift_z) : "(null)");
        return -1;
    }
    if (key_from_name("CTRL+KEY_LEFTSHIFT", &code) == 0 ||
        key_from_name("CTRL+CTRL+KEY_Z", &code) == 0 ||
        key_from_name("HYPER+KEY_Z", &code) == 0) {
        log_error("keys", "bad combo name was accepted");
        return -1;
    }

    combo_reset(&c);
    if (feed(&c, KEY_LEFTCTRL, 1, &v) != -1 || feed(&c, KEY_Z, 1, &v) != ctrl_z || v != 1 ||
        feed(&c, KEY_LEFTCTRL, 0, &v) != -1 || feed(&c, KEY_Z, 0, &v) != -1) {
        log_error("keys", "Ctrl+Z did not decode");
        return -1;
    }
    if (feed(&c, KEY_LEFTCTRL, 1, &v) != -1 || feed(&c, KEY_LEFTSHIFT, 1, &v) != -1 ||
        feed(&c, KEY_Z, 1, &v) != ctrl_shift_z || feed(&c, KEY_LEFTCTRL, 0, &v) != -1 ||
        feed(&c, KEY_LEFTSHIFT, 0, &v) != -1 || feed(&c, KEY_Z, 0, &v) != -1) {
        log_error("keys", "Ctrl+Shift+Z did not decode");
        return -1;
    }
    if (feed(&c, KEY_LEFTCTRL, 1, &v) != -1 || feed(&c, KEY_LEFTCTRL, 0, &v) != KEY_LEFTCTRL ||
        v != 1) {
        log_error("keys", "a lone Ctrl did not decode");
        return -1;
    }
    if (feed(&c, KEY_KP1, 1, &v) != KEY_KP1 || feed(&c, KEY_KP1, 2, &v) != KEY_KP1 || v != 2) {
        log_error("keys", "a plain key did not decode");
        return -1;
    }
    return 0;
}

int keys_self_test(void)
{
    int code = -1;

    if (key_from_name("KEY_KP0", &code) != 0 || code != KEY_KP0) {
        log_error("keys", "KEY_KP0 lookup failed");
        return -1;
    }
    if (key_from_name("KEY_KPPLUS", &code) != 0 || code != KEY_KPPLUS) {
        log_error("keys", "KEY_KPPLUS lookup failed");
        return -1;
    }
    if (key_from_name("NOT_A_KEY", &code) == 0) {
        log_error("keys", "unknown key name was accepted");
        return -1;
    }
    if (!key_name(KEY_KPDOT) || strcmp(key_name(KEY_KPDOT), "KEY_KPDOT") != 0) {
        log_error("keys", "key name lookup failed");
        return -1;
    }
    {
        int fds[32];
        int keypad = 0;
        int n;
        int i;
        int pads = 0;

        n = keys_open_capture(fds, 32, &keypad);
        if (n < 0) {
            log_error("keys", "keyboard capture open failed");
            return -1;
        }
        for (i = 0; i < n; i++) {
            char name[256];
            unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];

            memset(name, 0, sizeof name);
            memset(bits, 0, sizeof bits);
            if (ioctl(fds[i], EVIOCGNAME(sizeof name), name) < 0 ||
                ioctl(fds[i], EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) {
                log_error("keys", "cannot read capture device name");
                keys_close_capture(fds, n);
                return -1;
            }
            if (pad_device(fds[i], name, bits)) {
                if (i != pads) {
                    log_error("keys", "keypads were not the first capture devices");
                    keys_close_capture(fds, n);
                    return -1;
                }
                pads++;
            }
        }
        if (keypad != pads) {
            log_error("keys", "keypad count does not match the opened devices");
            keys_close_capture(fds, n);
            return -1;
        }
        keys_close_capture(fds, n);
    }
    {
        int pipefd[2];
        struct input_event ev;
        int code;

        if (pipe(pipefd) != 0) {
            log_error("keys", "cannot create a key-event pipe");
            return -1;
        }
        if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) != 0) {
            log_error("keys", "cannot set the key-event pipe nonblocking");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
        memset(&ev, 0, sizeof ev);
        ev.type = EV_KEY;
        ev.value = 1;
        ev.code = KEY_NUMLOCK;
        if (write(pipefd[1], &ev, sizeof ev) != (ssize_t)sizeof ev) {
            log_error("keys", "cannot write a key event");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
        ev.code = KEY_A;
        ev.value = 2;
        if (write(pipefd[1], &ev, sizeof ev) != (ssize_t)sizeof ev) {
            log_error("keys", "cannot write a key repeat");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
        ev.value = 1;
        if (write(pipefd[1], &ev, sizeof ev) != (ssize_t)sizeof ev) {
            log_error("keys", "cannot write a key press");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
        combo_reset(&g_cap[pipefd[0]]);
        code = keys_read_press(pipefd[0]);
        close(pipefd[0]);
        close(pipefd[1]);
        if (code != KEY_A) {
            log_error("keys", "key press read returned %d", code);
            return -1;
        }
    }
    if (debounce_self_test() != 0 || combo_self_test() != 0) {
        return -1;
    }
    return 0;
}
