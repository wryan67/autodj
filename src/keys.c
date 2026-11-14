#include "keys.h"

#include "config.h"
#include "log.h"
#include "midi.h"
#include "player.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
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

/*
 * In raw mode the XP-Pen ACK05 Shortcut Remote reports on its vendor HID
 * channel instead of sending keyboard shortcuts. That is the only way to
 * see the button in the center of the dial. Report 02 f0 holds the buttons
 * as a 16-bit little-endian bitmap in bytes 2-3 and the dial in byte 7
 * (1 clockwise, 2 counterclockwise). Buttons are named by position: top row
 * left to right with the tall button last, then the middle and bottom rows.
 */
enum {
    REMOTE_BASE = 0x1000,
    REMOTE_BUTTONS = 10,
    REMOTE_CENTER = REMOTE_BASE + REMOTE_BUTTONS,
    REMOTE_DIAL_CW,
    REMOTE_DIAL_CCW,
    REMOTE_END,
    RAW_REPORT = 12,
    RAW_REPEAT_DELAY_MS = 250,
    RAW_REPEAT_MS = 33,
    RAW_RETRY_MS = 3000
};

static const char *g_remote_names[REMOTE_END - REMOTE_BASE] = {
    "REMOTE_BTN1", "REMOTE_BTN2", "REMOTE_BTN3", "REMOTE_BTN4", "REMOTE_BTN5",
    "REMOTE_BTN6", "REMOTE_BTN7", "REMOTE_BTN8", "REMOTE_BTN9", "REMOTE_BTN10",
    "REMOTE_CENTER", "REMOTE_DIAL_CW", "REMOTE_DIAL_CCW"
};

/* Bitmap bit of each button, REMOTE_BTN1 first, then the center. */
static const int g_remote_bits[REMOTE_BUTTONS + 1] = {0, 1, 2, 6, 3, 4, 5, 7, 8, 9, 10};

/* Report that switches the remote to raw mode. */
static const unsigned char g_raw_magic[64] = {0x02, 0xb0, 0x04};

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
static int g_remote_kbd[MAX_PADS];  /* the remote's keyboard interface */
static int g_rep_key = -1;          /* raw button held for repeats */
static int g_ngrabbed;
static void (*g_jog_hook)(int direction, long long when_us);
/* What the dial does in each mode, and the mode it is in now. */
static int g_dial_modes[DIAL_SLOTS] = {DIAL_JOG, DIAL_NONE, DIAL_NONE};
static int g_dial_slot;
static long long g_jog_last_us;
static struct Combo g_cap[MAX_CAPTURE_FD];
/* Raw remote channels by fd, with the buttons held. */
static unsigned char g_is_raw[MAX_CAPTURE_FD];
static unsigned g_raw_held[MAX_CAPTURE_FD];
static struct timespec g_raw_since;   /* when the held button went down */
static struct timespec g_raw_repeat;  /* last repeat sent */
static struct timespec g_raw_retry;   /* last attempt to open the raw channel */
static int g_raw_wanted;              /* keys_open looks for the raw channel */
static struct timespec g_raw_kick;    /* last raw-mode request */
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

    if (code >= REMOTE_BASE && code < REMOTE_END) {
        return g_remote_names[code - REMOTE_BASE];
    }
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
    for (i = 0; i < REMOTE_END - REMOTE_BASE; i++) {
        if (strcasecmp(g_remote_names[i], name) == 0) {
            if (mods) {
                return -1;
            }
            *code = REMOTE_BASE + (int)i;
            return 0;
        }
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

static int is_raw(int fd)
{
    return fd >= 0 && fd < MAX_CAPTURE_FD && g_is_raw[fd];
}

static long ms_since(const struct timespec *then, const struct timespec *now)
{
    return (long)(now->tv_sec - then->tv_sec) * 1000 + (now->tv_nsec - then->tv_nsec) / 1000000;
}

/* Turn one raw report into presses (value 1) and releases (value 0).
 * *held is the button bitmap from the previous report. Returns the count. */
static int raw_parse(unsigned *held, const unsigned char *r, size_t n, int *keys, int *values, int max)
{
    unsigned now;
    int count = 0;
    int i;

    if (n < 8 || r[0] != 0x02 || r[1] != 0xf0) {
        return 0;
    }
    now = (unsigned)r[2] | ((unsigned)r[3] << 8);
    for (i = 0; i <= REMOTE_BUTTONS && count < max; i++) {
        unsigned bit = 1u << g_remote_bits[i];

        if ((now & bit) != (*held & bit)) {
            keys[count] = REMOTE_BASE + i;
            values[count++] = (now & bit) ? 1 : 0;
        }
    }
    *held = now;
    if ((r[7] == 1 || r[7] == 2) && count < max) {
        keys[count] = r[7] == 1 ? REMOTE_DIAL_CW : REMOTE_DIAL_CCW;
        values[count++] = 1;
    }
    return count;
}

static void raw_kick(int fd)
{
    if (write(fd, g_raw_magic, sizeof g_raw_magic) != (ssize_t)sizeof g_raw_magic) {
        log_warn("keys", "could not switch the shortcut remote to raw mode: %s", strerror(errno));
    }
    clock_gettime(CLOCK_MONOTONIC, &g_raw_kick);
}

static int read_text(const char *path, char *buf, size_t n)
{
    FILE *fp = fopen(path, "r");
    size_t got;

    if (!fp) {
        return -1;
    }
    got = fread(buf, 1, n - 1, fp);
    fclose(fp);
    buf[got] = '\0';
    return 0;
}

/* Open the vendor channel of the shortcut remote and switch it to raw mode.
 * Returns the fd, or -1. quiet leaves out the log lines. */
static int raw_open(int quiet)
{
    DIR *dir = opendir("/sys/class/hidraw");
    struct dirent *de;
    int fd = -1;

    if (!dir) {
        return -1;
    }
    while (fd < 0 && (de = readdir(dir)) != NULL) {
        char path[512];
        char text[2048];
        unsigned char desc[8];
        FILE *fp;
        size_t got;

        if (strncmp(de->d_name, "hidraw", 6) != 0) {
            continue;
        }
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", de->d_name);
        if (read_text(path, text, sizeof text) != 0 ||
            !strstr(text, "HID_ID=0003:000028BD:00000202")) {
            continue;
        }
        /* The vendor channel starts with usage page 0xff0a. */
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/report_descriptor", de->d_name);
        fp = fopen(path, "rb");
        if (!fp) {
            continue;
        }
        got = fread(desc, 1, sizeof desc, fp);
        fclose(fp);
        if (got < 3 || desc[0] != 0x06 || desc[1] != 0x0a || desc[2] != 0xff) {
            continue;
        }
        snprintf(path, sizeof path, "/dev/%s", de->d_name);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (!quiet) {
                log_warn("keys", "cannot open %s (%s), so the dial's center button is off. "
                         "Run ./build.sh once to install the udev rule.", path, strerror(errno));
            }
            break;
        }
        if (fd >= MAX_CAPTURE_FD) {
            close(fd);
            fd = -1;
            break;
        }
        g_is_raw[fd] = 1;
        g_raw_held[fd] = 0;
        raw_kick(fd);
        if (!quiet) {
            log_info("keys", "shortcut remote raw mode on %s", path);
        }
    }
    closedir(dir);
    return fd;
}

static void raw_close(int fd)
{
    if (is_raw(fd)) {
        g_is_raw[fd] = 0;
        g_raw_held[fd] = 0;
    }
}

int keys_grab(int fd)
{
    if (is_raw(fd)) {
        return 0;
    }
    if (fd < 0 || ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        return -1;
    }
    return 0;
}

void keys_ungrab(int fd)
{
    if (fd < 0 || is_raw(fd)) {
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
    if (is_raw(fd)) {
        unsigned char r[64];
        ssize_t n;

        while ((n = read(fd, r, sizeof r)) > 0) {
            int keys[16];
            int values[16];
            int count = raw_parse(&g_raw_held[fd], r, (size_t)n, keys, values, 16);
            int i;

            for (i = 0; i < count; i++) {
                if (found < 0 && values[i] == 1) {
                    found = keys[i];
                }
            }
        }
        return found;
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
    if (npads < (int)(sizeof pads / sizeof pads[0])) {
        int fd = raw_open(1);

        if (fd >= 0) {
            pads[npads++] = fd;
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
            raw_close(fds[i]);
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
    {
        static const int jog_only[DIAL_SLOTS] = {DIAL_JOG, DIAL_NONE, DIAL_NONE};

        memcpy(g_dial_modes, jog_only, sizeof g_dial_modes);
        g_dial_slot = 0;
    }
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
        g_remote_kbd[g_nfd] = strcmp(name, "USB Keypad") != 0;
        g_fds[g_nfd++] = fd;
    }
    g_raw_wanted = 1;
    clock_gettime(CLOCK_MONOTONIC, &g_raw_retry);
    if (g_nfd < MAX_PADS) {
        int fd = raw_open(0);

        if (fd >= 0) {
            g_remote_kbd[g_nfd] = 0;
            g_fds[g_nfd++] = fd;
            g_ngrabbed++;
        }
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
        keys_ungrab(g_fds[i]);
        raw_close(g_fds[i]);
        close(g_fds[i]);
        g_fds[i] = -1;
    }
    g_nfd = 0;
    g_ngrabbed = 0;
    g_raw_wanted = 0;
    g_rep_key = -1;
}

static int dial_mode_now(void)
{
    return g_dial_modes[g_dial_slot];
}

static void log_dial_mode(void)
{
    log_info("keys", "dial mode %s", config_dial_mode_name(dial_mode_now()));
}

void keys_set_dial_modes(const int *modes)
{
    int i;

    g_dial_slot = 0;
    for (i = 0; i < DIAL_SLOTS; i++) {
        g_dial_modes[i] = modes ? modes[i] : DIAL_NONE;
    }
    for (i = 0; i < DIAL_SLOTS; i++) {
        if (g_dial_modes[i] != DIAL_NONE) {
            g_dial_slot = i;
            break;
        }
    }
    log_dial_mode();
}

/* The next mode that is not none, after the current one. */
static void next_dial_mode(void)
{
    int i;

    for (i = 1; i <= DIAL_SLOTS; i++) {
        int slot = (g_dial_slot + i) % DIAL_SLOTS;

        if (g_dial_modes[slot] != DIAL_NONE) {
            g_dial_slot = slot;
            break;
        }
    }
    log_dial_mode();
}

static int raw_index(void)
{
    int i;

    for (i = 0; i < g_nfd; i++) {
        if (is_raw(g_fds[i])) {
            return i;
        }
    }
    return -1;
}

int keys_wait_ms(int max_ms)
{
    if (g_rep_key >= 0 && max_ms > RAW_REPEAT_MS) {
        return RAW_REPEAT_MS;
    }
    return max_ms;
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

static int is_step_action(int id)
{
    return id == KEYID_DIAL_FWD || id == KEYID_DIAL_BACK || id == KEYID_VOL_UP ||
           id == KEYID_VOL_DOWN || id == KEYID_JOG_UP || id == KEYID_JOG_DOWN ||
           id == KEYID_RV_UP || id == KEYID_RV_DOWN;
}

/* Jog by count clicks. A knob can report several clicks in one message, so
 * their times are spread back toward the previous one, which keeps the
 * player's measure of the turning speed. */
static void jog_steps(int dir, int count, long long us)
{
    long long span = us - g_jog_last_us;
    long long step;
    int i;

    if (count < 1) {
        count = 1;
    }
    if (g_jog_last_us <= 0 || span <= 0 || span > 100000) {
        span = 8000LL * count;
    }
    step = span / count;
    for (i = 0; i < count; i++) {
        long long t = us - (long long)(count - 1 - i) * step;

        player_jog(dir, t);
        if (g_jog_hook) {
            g_jog_hook(dir, t);
        }
    }
    g_jog_last_us = us;
}

/* A dial, volume, jog, or reverb action. percent is the volume per step.
 * The reverb knob moves twice that, so a knob covers its range in 50 steps. */
static void step_action(int id, int count, int percent, long long us)
{
    int dir = (id == KEYID_DIAL_FWD || id == KEYID_VOL_UP || id == KEYID_JOG_UP ||
               id == KEYID_RV_UP) ? 1 : -1;
    int mode;

    if (id == KEYID_VOL_UP || id == KEYID_VOL_DOWN) {
        mode = DIAL_VOLUME;
    } else if (id == KEYID_JOG_UP || id == KEYID_JOG_DOWN) {
        mode = DIAL_JOG;
    } else if (id == KEYID_RV_UP || id == KEYID_RV_DOWN) {
        mode = DIAL_REVERB;
    } else {
        mode = dial_mode_now();
    }
    if (mode == DIAL_VOLUME) {
        player_volume_step(dir, percent * count);
    } else if (mode == DIAL_JOG) {
        jog_steps(dir, count, us);
    } else if (mode == DIAL_REVERB) {
        player_reverb_step(dir, 2 * percent * count);
    }
}

static void press_action(int id)
{
    switch (id) {
    case KEYID_DIAL_MODE:
        next_dial_mode();
        break;
    case KEYID_PAUSE:
        player_toggle_pause();
        break;
    case KEYID_SKIP:
        player_skip();
        break;
    case KEYID_PREV:
        player_previous();
        break;
    case KEYID_REW05:
        player_rewind(500);
        break;
    case KEYID_REW10:
        player_rewind(1000);
        break;
    case KEYID_REW15:
        player_rewind(1500);
        break;
    case KEYID_REVERB:
        player_fx_toggle(PLAYER_FX_REVERB);
        break;
    case KEYID_PITCH:
        player_fx_toggle(PLAYER_FX_PITCH);
        break;
    case KEYID_AUTOPAN:
        player_fx_toggle(PLAYER_FX_AUTOPAN);
        break;
    case KEYID_PHASER:
        player_fx_toggle(PLAYER_FX_PHASER);
        break;
    case KEYID_CHORUS:
        player_fx_toggle(PLAYER_FX_CHORUS);
        break;
    case KEYID_FLANGER:
        player_fx_toggle(PLAYER_FX_FLANGER);
        break;
    case KEYID_EFFECTS_OFF:
        player_fx_all_off();
        break;
    default:
        break;
    }
}

static long long tv_us(const struct timeval *when)
{
    return (long long)when->tv_sec * 1000000LL + when->tv_usec;
}

/* Action bound to a keypad or remote code, or -1. */
static int pad_action(const struct Config *cfg, int key)
{
    const int codes[KEYID_COUNT] = {
        cfg->key_pause, cfg->key_skip, cfg->key_prev, cfg->key_rew05, cfg->key_rew10,
        cfg->key_rew15, cfg->key_dial_fwd, cfg->key_dial_back, cfg->key_dial_mode,
        cfg->key_reverb, cfg->key_pitch, cfg->key_autopan, cfg->key_phaser,
        cfg->key_chorus, cfg->key_flanger, cfg->key_effects_off, cfg->key_vol_up,
        cfg->key_vol_down, cfg->key_jog_up, cfg->key_jog_down, cfg->key_rv_up,
        cfg->key_rv_down
    };
    int id;

    for (id = 0; id < KEYID_COUNT; id++) {
        if (codes[id] >= 0 && codes[id] == key) {
            return id;
        }
    }
    return -1;
}

static void dispatch(const struct Config *cfg, int key, int value, const struct timeval *when)
{
    const char *name;
    int id;

    if ((key & KEY_CODE_MASK) == KEY_NUMLOCK) {
        return;
    }
    id = pad_action(cfg, key);
    /* Dial clicks come faster than the press gap when it spins, and the
     * player measures their spacing, so they skip the debounce. A held key
     * repeats them. The keypad and remote change the volume by 1. */
    if (id >= 0 && is_step_action(id)) {
        if (value == 1 || value == 2) {
            step_action(id, 1, 1, tv_us(when));
        }
        return;
    }
    /* Value 1 is a press. Key repeat (value 2) does not fire an action. */
    if (value != 1 || !press_accepted(when)) {
        return;
    }
    if (id >= 0) {
        press_action(id);
        return;
    }
    name = key_name(key);
    log_info("keys", "key %s is not mapped", name ? name : "unknown");
}

int keys_midi_dispatch(const struct Config *cfg, int code, int steps, const struct timeval *when)
{
    int id;

    if (!cfg || !when || code < 0) {
        return 0;
    }
    for (id = 0; id < KEYID_COUNT; id++) {
        if (cfg->midi_code[id] == code) {
            break;
        }
    }
    if (id >= KEYID_COUNT) {
        return 0;
    }
    /* A knob changes the volume by 1 a step, and a pad by 5 like a key. */
    if (is_step_action(id)) {
        if (midi_is_knob(code)) {
            step_action(id, steps, 1, tv_us(when));
        } else {
            step_action(id, 1, 5, tv_us(when));
        }
        return 1;
    }
    if (press_accepted(when)) {
        press_action(id);
    }
    return 1;
}

static void drop_fd(int i)
{
    keys_ungrab(g_fds[i]);
    raw_close(g_fds[i]);
    close(g_fds[i]);
    memmove(&g_fds[i], &g_fds[i + 1], (size_t)(g_nfd - i - 1) * sizeof g_fds[0]);
    memmove(&g_combo[i], &g_combo[i + 1], (size_t)(g_nfd - i - 1) * sizeof g_combo[0]);
    memmove(&g_remote_kbd[i], &g_remote_kbd[i + 1], (size_t)(g_nfd - i - 1) * sizeof g_remote_kbd[0]);
    g_nfd--;
}

/* Returns the last read result, so a lost device shows as -1. */
static ssize_t poll_raw(const struct Config *cfg, int fd)
{
    unsigned char r[64];
    ssize_t n;

    while ((n = read(fd, r, sizeof r)) > 0) {
        int keys[16];
        int values[16];
        int count = raw_parse(&g_raw_held[fd], r, (size_t)n, keys, values, 16);
        struct timeval when;
        int k;

        gettimeofday(&when, NULL);
        for (k = 0; k < count; k++) {
            if (values[k] == 1) {
                dispatch(cfg, keys[k], 1, &when);
                if (keys[k] < REMOTE_DIAL_CW) {
                    g_rep_key = keys[k];
                    clock_gettime(CLOCK_MONOTONIC, &g_raw_since);
                    g_raw_repeat = g_raw_since;
                }
            } else if (keys[k] == g_rep_key) {
                g_rep_key = -1;
            }
        }
    }
    return n;
}

void keys_poll(const struct Config *cfg)
{
    struct input_event ev;
    struct timespec now;
    int i;

    if (!cfg) {
        return;
    }
    for (i = 0; i < g_nfd; i++) {
        ssize_t n;

        if (is_raw(g_fds[i])) {
            n = poll_raw(cfg, g_fds[i]);
        } else {
            while ((n = read(g_fds[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                int value = 0;
                int key = combo_feed(&g_combo[i], &ev, &value);

                /* A shortcut from the remote means it left raw mode, for
                 * example after it slept. Ask again. */
                if (g_remote_kbd[i] && ev.type == EV_KEY && raw_index() >= 0) {
                    struct timespec t;

                    clock_gettime(CLOCK_MONOTONIC, &t);
                    if (ms_since(&g_raw_kick, &t) >= 1000) {
                        log_info("keys", "shortcut remote left raw mode, switching it back");
                        raw_kick(g_fds[raw_index()]);
                    }
                }
                if (key >= 0) {
                    dispatch(cfg, key, value, &ev.time);
                }
            }
        }
        if (n < 0 && (errno == ENODEV || errno == EIO)) {
            log_warn("keys", "a key device was disconnected");
            drop_fd(i);
            i--;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (g_rep_key >= 0 && raw_index() < 0) {
        g_rep_key = -1;
    }
    if (g_rep_key >= 0 && ms_since(&g_raw_since, &now) >= RAW_REPEAT_DELAY_MS &&
        ms_since(&g_raw_repeat, &now) >= RAW_REPEAT_MS) {
        struct timeval when;

        gettimeofday(&when, NULL);
        g_raw_repeat = now;
        dispatch(cfg, g_rep_key, 2, &when);
    }
    /* Reopen the raw channel after the remote was plugged back in. */
    if (g_raw_wanted && raw_index() < 0 && g_nfd < MAX_PADS &&
        ms_since(&g_raw_retry, &now) >= RAW_RETRY_MS) {
        int fd;

        g_raw_retry = now;
        fd = raw_open(1);
        if (fd >= 0) {
            log_info("keys", "shortcut remote raw mode on again");
            g_remote_kbd[g_nfd] = 0;
            g_combo[g_nfd].mods = 0;
            g_combo[g_nfd].lone = -1;
            g_fds[g_nfd++] = fd;
            g_ngrabbed++;
        }
    }
}

static int g_kbd_fd = -1;
static int g_kbd_raw;
static int g_kbd_flags;
static struct termios g_kbd_saved;
static unsigned char g_kbd_buf[16];
static int g_kbd_len;
static struct timeval g_kbd_partial;

enum { KBD_PARTIAL_MS = 50 };

static int kbd_eq(const char *bound, const char *name)
{
    return bound && bound[0] && name && name[0] && strcmp(bound, name) == 0;
}

/* Action bound to a terminal key, or -1. */
static int kbd_action(const struct Config *cfg, const char *name)
{
    const char *keys[KEYID_COUNT] = {
        cfg->kbd_pause, cfg->kbd_skip, cfg->kbd_prev, cfg->kbd_rew05, cfg->kbd_rew10,
        cfg->kbd_rew15, cfg->kbd_dial_fwd, cfg->kbd_dial_back, cfg->kbd_dial_mode,
        cfg->kbd_reverb, cfg->kbd_pitch, cfg->kbd_autopan, cfg->kbd_phaser,
        cfg->kbd_chorus, cfg->kbd_flanger, cfg->kbd_effects_off, cfg->kbd_vol_up,
        cfg->kbd_vol_down, cfg->kbd_jog_up, cfg->kbd_jog_down, cfg->kbd_rv_up,
        cfg->kbd_rv_down
    };
    int id;

    for (id = 0; id < KEYID_COUNT; id++) {
        if (kbd_eq(keys[id], name)) {
            return id;
        }
    }
    return -1;
}

void keys_kbd_dispatch(const struct Config *cfg, const char *name)
{
    struct timeval when;
    int id;

    if (!cfg || !name || !name[0]) {
        return;
    }
    gettimeofday(&when, NULL);
    id = kbd_action(cfg, name);
    /* Dial, volume, and jog keys repeat. A keyboard press changes the volume by 5. */
    if (id >= 0 && is_step_action(id)) {
        step_action(id, 1, 5, tv_us(&when));
        return;
    }
    if (!press_accepted(&when)) {
        return;
    }
    if (id >= 0) {
        press_action(id);
        return;
    }
    log_info("keys", "key %s is not mapped", name);
}

int keys_kbd_canon(const char *name, char *dest, size_t n)
{
    static const struct {
        const char *word;
        const char *canon;
    } words[] = {
        {"space", "Space"},
        {"enter", "Enter"},
        {"tab", "Tab"},
        {"backspace", "Backspace"},
        {"delete", "Delete"},
        {"up", "Up"},
        {"down", "Down"},
        {"left", "Left"},
        {"right", "Right"},
        {"home", "Home"},
        {"end", "End"},
    };
    char buf[32];
    const char *p;
    size_t len;
    int only_space = 1;
    int saw_space = 0;
    int i;

    if (!dest || n == 0) {
        return -1;
    }
    /* name and dest may be the same buffer. Read the whole name before
     * writing dest, or a typed letter is wiped and then rejected. */
    if (!name) {
        dest[0] = '\0';
        return -1;
    }
    for (p = name; *p; p++) {
        if (*p != ' ' && *p != '\t') {
            only_space = 0;
            break;
        }
        if (*p == ' ') {
            saw_space = 1;
        }
    }
    if (only_space && saw_space) {
        if (n < 6) {
            dest[0] = '\0';
            return -1;
        }
        snprintf(dest, n, "Space");
        return 0;
    }
    while (*name && isspace((unsigned char)*name)) {
        name++;
    }
    len = strlen(name);
    while (len > 0 && isspace((unsigned char)name[len - 1])) {
        len--;
    }
    if (len == 0 || len >= sizeof buf || len >= n) {
        dest[0] = '\0';
        return -1;
    }
    memcpy(buf, name, len);
    buf[len] = '\0';
    dest[0] = '\0';
    if ((len >= 4 && strncasecmp(buf, "KEY_", 4) == 0) ||
        (len >= 7 && strncasecmp(buf, "REMOTE_", 7) == 0) ||
        strchr(buf, '+') != NULL) {
        return -1;
    }
    if (len == 1) {
        unsigned char c = (unsigned char)buf[0];

        if (c >= 33 && c <= 126) {
            dest[0] = (char)c;
            dest[1] = '\0';
            return 0;
        }
        return -1;
    }
    for (i = 0; i < (int)(sizeof words / sizeof words[0]); i++) {
        if (strcasecmp(buf, words[i].word) == 0) {
            snprintf(dest, n, "%s", words[i].canon);
            return 0;
        }
    }
    {
        unsigned char c = (unsigned char)buf[0];
        int need = 1;

        if ((c & 0xe0) == 0xc0) {
            need = 2;
        } else if ((c & 0xf0) == 0xe0) {
            need = 3;
        } else if ((c & 0xf8) == 0xf0) {
            need = 4;
        } else {
            return -1;
        }
        if ((int)len != need || need >= (int)n) {
            return -1;
        }
        for (i = 1; i < need; i++) {
            if (((unsigned char)buf[i] & 0xc0) != 0x80) {
                return -1;
            }
        }
        memcpy(dest, buf, (size_t)need);
        dest[need] = '\0';
        return 0;
    }
}

static void kbd_shift(int count)
{
    if (count <= 0) {
        return;
    }
    if (count >= g_kbd_len) {
        g_kbd_len = 0;
        return;
    }
    memmove(g_kbd_buf, g_kbd_buf + count, (size_t)(g_kbd_len - count));
    g_kbd_len -= count;
}

static const char *kbd_arrow(unsigned char c)
{
    switch (c) {
    case 'A':
        return "Up";
    case 'B':
        return "Down";
    case 'C':
        return "Right";
    case 'D':
        return "Left";
    case 'H':
        return "Home";
    case 'F':
        return "End";
    default:
        return NULL;
    }
}

/* Bytes used by the escape sequence at the front of the buffer, or 0 when
 * more bytes are required. A finished sequence that is not a known key is
 * consumed and ignored. */
static int kbd_take_seq(const struct Config *cfg)
{
    const char *name = NULL;
    int i;

    if (g_kbd_len < 2 || g_kbd_buf[0] != 0x1b) {
        return 0;
    }
    if (g_kbd_buf[1] == 'O') {
        if (g_kbd_len < 3) {
            return 0;
        }
        name = kbd_arrow(g_kbd_buf[2]);
        if (name) {
            keys_kbd_dispatch(cfg, name);
        }
        return 3;
    }
    if (g_kbd_buf[1] != '[') {
        return 0;
    }
    for (i = 2; i < g_kbd_len; i++) {
        unsigned char c = g_kbd_buf[i];

        if (c < 0x40 || c > 0x7e) {
            continue;
        }
        name = kbd_arrow(c);
        if (!name && c == '~') {
            int num = 0;
            int any = 0;
            int j;

            for (j = 2; j < i && g_kbd_buf[j] >= '0' && g_kbd_buf[j] <= '9'; j++) {
                any = 1;
                num = num * 10 + (g_kbd_buf[j] - '0');
            }
            if (any && (num == 1 || num == 7)) {
                name = "Home";
            } else if (any && num == 3) {
                name = "Delete";
            } else if (any && (num == 4 || num == 8)) {
                name = "End";
            }
        }
        if (name) {
            keys_kbd_dispatch(cfg, name);
        }
        return i + 1;
    }
    return 0;
}

static int kbd_utf8_need(unsigned char c)
{
    if ((c & 0xe0) == 0xc0) {
        return 2;
    }
    if ((c & 0xf0) == 0xe0) {
        return 3;
    }
    if ((c & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

static void kbd_emit_text(const struct Config *cfg, const unsigned char *s, int n)
{
    char raw[AUTODJ_KBD_LEN];
    char name[AUTODJ_KBD_LEN];

    if (n <= 0 || n >= (int)sizeof raw) {
        return;
    }
    memcpy(raw, s, (size_t)n);
    raw[n] = '\0';
    if (keys_kbd_canon(raw, name, sizeof name) != 0) {
        return;
    }
    keys_kbd_dispatch(cfg, name);
}

static void kbd_parse(const struct Config *cfg, int timed_out)
{
    while (g_kbd_len > 0) {
        unsigned char c = g_kbd_buf[0];

        if (c == 0x1b) {
            if (g_kbd_len >= 2 && (g_kbd_buf[1] == '[' || g_kbd_buf[1] == 'O')) {
                int used = kbd_take_seq(cfg);

                if (used > 0) {
                    kbd_shift(used);
                    continue;
                }
                if (timed_out || g_kbd_len >= (int)sizeof g_kbd_buf) {
                    kbd_shift(g_kbd_len);
                    continue;
                }
                return;
            }
            if (g_kbd_len == 1) {
                if (!timed_out) {
                    return;
                }
                kbd_shift(1);
                continue;
            }
            kbd_shift(1);
            continue;
        }
        if (c == '\r' || c == '\n') {
            keys_kbd_dispatch(cfg, "Enter");
            kbd_shift(1);
            continue;
        }
        if (c == '\t') {
            keys_kbd_dispatch(cfg, "Tab");
            kbd_shift(1);
            continue;
        }
        if (c == 8 || c == 127) {
            keys_kbd_dispatch(cfg, "Backspace");
            kbd_shift(1);
            continue;
        }
        if (c >= 32 && c < 127) {
            kbd_emit_text(cfg, g_kbd_buf, 1);
            kbd_shift(1);
            continue;
        }
        if (c >= 0x80) {
            int need = kbd_utf8_need(c);
            int i;

            if (need == 1) {
                kbd_shift(1);
                continue;
            }
            if (g_kbd_len < need) {
                if (timed_out) {
                    kbd_shift(1);
                    continue;
                }
                return;
            }
            for (i = 1; i < need; i++) {
                if ((g_kbd_buf[i] & 0xc0) != 0x80) {
                    need = 1;
                    break;
                }
            }
            if (need > 1) {
                kbd_emit_text(cfg, g_kbd_buf, need);
            }
            kbd_shift(need);
            continue;
        }
        kbd_shift(1);
    }
}

int keys_kbd_open(void)
{
    struct termios raw;
    int flags;

    if (g_kbd_raw) {
        return 0;
    }
    if (!isatty(STDIN_FILENO)) {
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_kbd_saved) != 0) {
        return -1;
    }
    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    raw = g_kbd_saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return -1;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_kbd_saved);
        return -1;
    }
    g_kbd_flags = flags;
    g_kbd_fd = STDIN_FILENO;
    g_kbd_raw = 1;
    g_kbd_len = 0;
    return 0;
}

void keys_kbd_close(void)
{
    if (!g_kbd_raw) {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &g_kbd_saved);
    fcntl(STDIN_FILENO, F_SETFL, g_kbd_flags);
    g_kbd_raw = 0;
    g_kbd_fd = -1;
    g_kbd_len = 0;
}

int keys_kbd_fd(void)
{
    return g_kbd_raw ? g_kbd_fd : -1;
}

void keys_kbd_poll(const struct Config *cfg)
{
    unsigned char tmp[64];
    int got = 0;
    ssize_t n;

    if (!cfg || g_kbd_fd < 0) {
        return;
    }
    while ((n = read(g_kbd_fd, tmp, sizeof tmp)) > 0) {
        int i;

        got = 1;
        for (i = 0; i < n; i++) {
            if (g_kbd_len == 0) {
                gettimeofday(&g_kbd_partial, NULL);
            }
            if (g_kbd_len >= (int)sizeof g_kbd_buf) {
                kbd_parse(cfg, 1);
            }
            if (g_kbd_len < (int)sizeof g_kbd_buf) {
                g_kbd_buf[g_kbd_len++] = tmp[i];
            }
        }
    }
    if (!got && g_kbd_len > 0) {
        struct timeval now;

        gettimeofday(&now, NULL);
        kbd_parse(cfg, timeval_delta_ms(&now, &g_kbd_partial) >= KBD_PARTIAL_MS);
        return;
    }
    kbd_parse(cfg, 0);
}

static void kbd_feed(const struct Config *cfg, const char *bytes, int timed_out)
{
    size_t i;

    for (i = 0; bytes && bytes[i]; i++) {
        if (g_kbd_len == 0) {
            gettimeofday(&g_kbd_partial, NULL);
        }
        if (g_kbd_len < (int)sizeof g_kbd_buf) {
            g_kbd_buf[g_kbd_len++] = (unsigned char)bytes[i];
        }
    }
    kbd_parse(cfg, timed_out);
}

/* Test helper: the dial starts in first, and dial_mode switches to second. */
static void dial_test_modes(int first, int second)
{
    g_dial_modes[0] = first;
    g_dial_modes[1] = second;
    g_dial_modes[2] = DIAL_NONE;
    g_dial_slot = 0;
}

static int kbd_self_test(void)
{
    Config cfg;
    char name[AUTODJ_KBD_LEN];

    g_kbd_len = 0;
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    if (keys_kbd_canon("p", name, sizeof name) != 0 || strcmp(name, "p") != 0 ||
        keys_kbd_canon("P", name, sizeof name) != 0 || strcmp(name, "P") != 0 ||
        keys_kbd_canon(" space ", name, sizeof name) != 0 || strcmp(name, "Space") != 0 ||
        keys_kbd_canon("ENTER", name, sizeof name) != 0 || strcmp(name, "Enter") != 0 ||
        keys_kbd_canon("Backspace", name, sizeof name) != 0 ||
        strcmp(name, "Backspace") != 0 ||
        keys_kbd_canon("KEY_A", name, sizeof name) == 0 ||
        keys_kbd_canon("REMOTE_BTN1", name, sizeof name) == 0 ||
        keys_kbd_canon("", name, sizeof name) == 0 ||
        keys_kbd_canon("NotAKey", name, sizeof name) == 0) {
        log_error("keys", "keyboard name was not canonical");
        return -1;
    }
    snprintf(name, sizeof name, "q");
    if (keys_kbd_canon(name, name, sizeof name) != 0 || strcmp(name, "q") != 0) {
        log_error("keys", "a keyboard name cleared itself");
        return -1;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.key_pause = -1;
    cfg.key_skip = -1;
    cfg.key_prev = -1;
    cfg.key_rew05 = -1;
    cfg.key_rew10 = -1;
    cfg.key_rew15 = -1;
    cfg.key_dial_fwd = -1;
    cfg.key_dial_back = -1;
    cfg.key_dial_mode = -1;
    cfg.key_reverb = -1;
    cfg.key_pitch = -1;
    cfg.key_autopan = -1;
    cfg.key_phaser = -1;
    cfg.key_chorus = -1;
    cfg.key_flanger = -1;
    cfg.key_effects_off = -1;
    cfg.key_vol_up = -1;
    cfg.key_vol_down = -1;
    cfg.key_jog_up = -1;
    cfg.key_jog_down = -1;
    cfg.key_rv_up = -1;
    cfg.key_rv_down = -1;
    snprintf(cfg.kbd_reverb, sizeof cfg.kbd_reverb, "r");
    player_set_fx(NULL);
    player_set_volume_percent(50);
    dial_test_modes(DIAL_VOLUME, DIAL_JOG);
    g_press_seen = 0;
    keys_kbd_dispatch(&cfg, "r");
    if (!player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "keyboard r did not turn reverb on");
        return -1;
    }
    g_press_seen = 0;
    keys_kbd_dispatch(&cfg, "R");
    if (!player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "keyboard R matched r");
        return -1;
    }

    snprintf(cfg.kbd_dial_fwd, sizeof cfg.kbd_dial_fwd, "+");
    snprintf(cfg.kbd_dial_back, sizeof cfg.kbd_dial_back, "-");
    keys_kbd_dispatch(&cfg, "+");
    keys_kbd_dispatch(&cfg, "+");
    if (player_volume_percent() != 60) {
        log_error("keys", "keyboard dial left volume %d, expected 60",
                  player_volume_percent());
        return -1;
    }
    keys_kbd_dispatch(&cfg, "-");
    if (player_volume_percent() != 55) {
        log_error("keys", "keyboard dial down left volume %d, expected 55",
                  player_volume_percent());
        return -1;
    }

    snprintf(cfg.kbd_effects_off, sizeof cfg.kbd_effects_off, "x");
    player_fx_toggle(PLAYER_FX_CHORUS);
    g_press_seen = 0;
    keys_kbd_dispatch(&cfg, "x");
    if (player_fx_enabled(PLAYER_FX_REVERB) || player_fx_enabled(PLAYER_FX_CHORUS)) {
        log_error("keys", "keyboard all-off left an effect on");
        return -1;
    }
    player_fx_toggle(PLAYER_FX_FLANGER);
    g_press_seen = 0;
    keys_kbd_dispatch(&cfg, "x");
    if (player_fx_enabled(PLAYER_FX_FLANGER)) {
        log_error("keys", "a second keyboard all-off left flanger on");
        return -1;
    }

    snprintf(cfg.kbd_reverb, sizeof cfg.kbd_reverb, "Up");
    g_kbd_len = 0;
    g_press_seen = 0;
    kbd_feed(&cfg, "\033", 0);
    if (g_kbd_len != 1 || player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "a partial Esc was treated as a key");
        g_kbd_len = 0;
        return -1;
    }
    kbd_parse(&cfg, 1);
    if (g_kbd_len != 0 || player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "a timed-out Esc became a key");
        g_kbd_len = 0;
        return -1;
    }
    kbd_feed(&cfg, "\033[A", 0);
    if (!player_fx_enabled(PLAYER_FX_REVERB) || g_kbd_len != 0) {
        log_error("keys", "arrow sequence did not turn reverb on");
        g_kbd_len = 0;
        return -1;
    }
    snprintf(cfg.kbd_reverb, sizeof cfg.kbd_reverb, "Delete");
    g_press_seen = 0;
    kbd_feed(&cfg, "\033[3~", 0);
    if (player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "Delete sequence did not turn reverb off");
        g_kbd_len = 0;
        return -1;
    }

    /* Letters and digits are one byte. Enter and arrows are not, so this
     * used to clear the character before it could match. */
    snprintf(cfg.kbd_reverb, sizeof cfg.kbd_reverb, "q");
    snprintf(cfg.kbd_pitch, sizeof cfg.kbd_pitch, "1");
    snprintf(cfg.kbd_autopan, sizeof cfg.kbd_autopan, "Space");
    player_set_fx(NULL);
    g_kbd_len = 0;
    g_press_seen = 0;
    kbd_feed(&cfg, "q", 0);
    if (!player_fx_enabled(PLAYER_FX_REVERB) || g_kbd_len != 0) {
        log_error("keys", "typed q did not turn reverb on");
        g_kbd_len = 0;
        return -1;
    }
    g_press_seen = 0;
    kbd_feed(&cfg, "1", 0);
    if (!player_fx_enabled(PLAYER_FX_PITCH) || g_kbd_len != 0) {
        log_error("keys", "typed 1 did not turn chipmunk on");
        g_kbd_len = 0;
        return -1;
    }
    g_press_seen = 0;
    kbd_feed(&cfg, " ", 0);
    if (!player_fx_enabled(PLAYER_FX_AUTOPAN) || g_kbd_len != 0) {
        log_error("keys", "typed space did not turn autopan on");
        g_kbd_len = 0;
        return -1;
    }

    g_kbd_len = 0;
    dial_test_modes(DIAL_JOG, DIAL_NONE);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    player_set_fx(NULL);
    player_set_volume_percent(100);
    return 0;
}

/* MIDI controls run the same actions, and the dial cycles through its modes. */
static int midi_dispatch_self_test(void)
{
    static const int modes[DIAL_SLOTS] = {DIAL_VOLUME, DIAL_NONE, DIAL_JOG};
    Config cfg;
    struct timeval when = {100, 0};
    int id;
    int rc = -1;

    memset(&cfg, 0, sizeof cfg);
    cfg.key_pause = cfg.key_skip = cfg.key_prev = cfg.key_rew05 = -1;
    cfg.key_rew10 = cfg.key_rew15 = cfg.key_dial_fwd = cfg.key_dial_back = -1;
    cfg.key_dial_mode = cfg.key_reverb = cfg.key_pitch = cfg.key_autopan = -1;
    cfg.key_phaser = cfg.key_chorus = cfg.key_flanger = cfg.key_effects_off = -1;
    cfg.key_vol_up = KEY_U;
    cfg.key_vol_down = cfg.key_jog_up = cfg.key_jog_down = -1;
    cfg.key_rv_up = cfg.key_rv_down = -1;
    for (id = 0; id < KEYID_COUNT; id++) {
        cfg.midi_code[id] = -1;
    }
    cfg.midi_code[KEYID_VOL_UP] = MIDI_CC_UP | 3;
    cfg.midi_code[KEYID_VOL_DOWN] = MIDI_NOTE | 40;
    cfg.midi_code[KEYID_REVERB] = MIDI_NOTE | 36;
    cfg.midi_code[KEYID_DIAL_MODE] = MIDI_NOTE | 37;
    cfg.midi_code[KEYID_DIAL_FWD] = MIDI_CC_UP | 9;
    player_set_volume_percent(50);
    player_set_fx(NULL);
    g_press_seen = 0;
    keys_set_dial_modes(modes);
    if (dial_mode_now() != DIAL_VOLUME ||
        keys_midi_dispatch(&cfg, MIDI_CC_UP | 3, 3, &when) != 1 ||
        player_volume_percent() != 53 ||
        keys_midi_dispatch(&cfg, MIDI_NOTE | 40, 1, &when) != 1 ||
        player_volume_percent() != 48 ||
        keys_midi_dispatch(&cfg, MIDI_CC_DOWN | 3, 1, &when) != 0 ||
        keys_midi_dispatch(&cfg, MIDI_CC_UP | 9, 2, &when) != 1 ||
        player_volume_percent() != 50) {
        log_error("keys", "MIDI volume left %d%%", player_volume_percent());
        goto out;
    }
    if (keys_midi_dispatch(&cfg, MIDI_NOTE | 36, 1, &when) != 1 ||
        !player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "a MIDI pad did not turn reverb on");
        goto out;
    }
    /* Mode 2 is none, so the next mode is jog, and then volume again. */
    when.tv_sec = 101;
    keys_midi_dispatch(&cfg, MIDI_NOTE | 37, 1, &when);
    if (dial_mode_now() != DIAL_JOG) {
        log_error("keys", "dial mode did not skip none");
        goto out;
    }
    when.tv_sec = 102;
    keys_midi_dispatch(&cfg, MIDI_NOTE | 37, 1, &when);
    if (dial_mode_now() != DIAL_VOLUME) {
        log_error("keys", "dial mode did not wrap to the first mode");
        goto out;
    }
    /* The reverb knob moves 2% a step. The dial in reverb mode does the same. */
    cfg.midi_code[KEYID_RV_DOWN] = MIDI_CC_DOWN | 12;
    keys_midi_dispatch(&cfg, MIDI_CC_DOWN | 12, 3, &when);
    dial_test_modes(DIAL_REVERB, DIAL_NONE);
    keys_midi_dispatch(&cfg, MIDI_CC_UP | 9, 1, &when);
    if (player_reverb_percent() != 46) {
        log_error("keys", "reverb knob left %d%%", player_reverb_percent());
        goto out;
    }
    /* A volume key works whatever mode the dial is in, a keypad step is 1. */
    dial_test_modes(DIAL_JOG, DIAL_NONE);
    dispatch(&cfg, KEY_U, 1, &when);
    dispatch(&cfg, KEY_U, 2, &when);
    if (player_volume_percent() != 52) {
        log_error("keys", "keypad volume up left %d%%", player_volume_percent());
        goto out;
    }
    rc = 0;
out:
    dial_test_modes(DIAL_JOG, DIAL_NONE);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    player_set_fx(NULL);
    player_set_volume_percent(100);
    return rc;
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
    cfg.key_dial_fwd = -1;
    cfg.key_dial_back = -1;
    cfg.key_dial_mode = -1;
    cfg.key_reverb = KEY_KPPLUS;
    cfg.key_pitch = -1;
    cfg.key_autopan = -1;
    cfg.key_phaser = -1;
    cfg.key_chorus = -1;
    cfg.key_flanger = -1;
    cfg.key_effects_off = -1;
    cfg.key_vol_up = -1;
    cfg.key_vol_down = -1;
    cfg.key_jog_up = -1;
    cfg.key_jog_down = -1;
    cfg.key_rv_up = -1;
    cfg.key_rv_down = -1;
    player_set_volume_percent(50);
    player_set_fx(NULL);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
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
    if (!player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "first press did not turn reverb on");
        goto out;
    }

    /* 20 ms later is inside the gap. */
    if (put_key(pipefd[1], 10030, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a close press");
        goto out;
    }
    keys_poll(&cfg);
    if (!player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "close press turned reverb off");
        goto out;
    }

    /* Another key starts the gap again. */
    if (put_key(pipefd[1], 10070, KEY_A, 1) != 0 ||
        put_key(pipefd[1], 10090, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a pause press");
        goto out;
    }
    keys_poll(&cfg);
    if (!player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "press after pause turned reverb off");
        goto out;
    }

    /* 30 ms after the previous press is accepted. */
    if (put_key(pipefd[1], 10120, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a later press");
        goto out;
    }
    keys_poll(&cfg);
    if (player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "press at 30 ms left reverb on");
        goto out;
    }

    /* A repeat is not another press, so it does not toggle the effect. */
    if (put_key(pipefd[1], 10140, KEY_KPPLUS, 2) != 0 ||
        put_key(pipefd[1], 10145, KEY_KPPLUS, 1) != 0) {
        log_error("keys", "cannot write a key repeat");
        goto out;
    }
    keys_poll(&cfg);
    if (player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "a repeat turned reverb on");
        goto out;
    }

    /* In volume mode each dial click is one step, even when they come fast. */
    cfg.key_dial_mode = KEY_C;
    cfg.key_dial_fwd = KEY_D;
    cfg.key_dial_back = KEY_E;
    dial_test_modes(DIAL_JOG, DIAL_VOLUME);
    if (put_key(pipefd[1], 10200, KEY_C, 1) != 0 ||
        put_key(pipefd[1], 10205, KEY_D, 1) != 0 ||
        put_key(pipefd[1], 10206, KEY_D, 1) != 0 ||
        put_key(pipefd[1], 10207, KEY_E, 1) != 0) {
        log_error("keys", "cannot write dial events");
        goto out;
    }
    keys_poll(&cfg);
    if (dial_mode_now() != DIAL_VOLUME || player_volume_percent() != 51) {
        log_error("keys", "dial in volume mode left volume %d, expected 51",
                  player_volume_percent());
        goto out;
    }

    /* One button turns every effect off, and does not turn them back on. */
    cfg.key_effects_off = KEY_F;
    player_fx_toggle(PLAYER_FX_REVERB);
    player_fx_toggle(PLAYER_FX_CHORUS);
    player_fx_toggle(PLAYER_FX_PHASER);
    if (!player_fx_enabled(PLAYER_FX_REVERB) || !player_fx_enabled(PLAYER_FX_CHORUS) ||
        !player_fx_enabled(PLAYER_FX_PHASER)) {
        log_error("keys", "could not turn effects on for the all-off button");
        goto out;
    }
    if (put_key(pipefd[1], 10300, KEY_F, 1) != 0) {
        log_error("keys", "cannot write the all-off press");
        goto out;
    }
    keys_poll(&cfg);
    if (player_fx_enabled(PLAYER_FX_REVERB) || player_fx_enabled(PLAYER_FX_CHORUS) ||
        player_fx_enabled(PLAYER_FX_PHASER) || player_fx_enabled(PLAYER_FX_FLANGER)) {
        log_error("keys", "all-off left an effect on");
        goto out;
    }
    player_fx_toggle(PLAYER_FX_FLANGER);
    if (put_key(pipefd[1], 10340, KEY_F, 1) != 0) {
        log_error("keys", "cannot write a second all-off press");
        goto out;
    }
    keys_poll(&cfg);
    if (player_fx_enabled(PLAYER_FX_FLANGER) || player_fx_enabled(PLAYER_FX_REVERB)) {
        log_error("keys", "a second all-off press left an effect on");
        goto out;
    }
    rc = 0;
out:
    dial_test_modes(DIAL_JOG, DIAL_NONE);
    g_fds[0] = -1;
    g_nfd = 0;
    close(pipefd[0]);
    close(pipefd[1]);
    g_press_seen = 0;
    memset(&g_press_time, 0, sizeof g_press_time);
    player_set_fx(NULL);
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

            if (is_raw(fds[i])) {
                if (i != pads) {
                    log_error("keys", "keypads were not the first capture devices");
                    keys_close_capture(fds, n);
                    return -1;
                }
                pads++;
                continue;
            }
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
    {
        static const unsigned char press[12] = {0x02, 0xf0, 0x40, 0x04};
        static const unsigned char up[12] = {0x02, 0xf0};
        static const unsigned char ccw[12] = {0x02, 0xf0, 0, 0, 0, 0, 0, 2};
        static const unsigned char battery[12] = {0x02, 0xf2, 0x01, 0x50};
        unsigned held = 0;
        int keys[16];
        int values[16];
        int code = -1;

        /* Bit 6 is the tall button (REMOTE_BTN4), bit 10 the center. */
        if (raw_parse(&held, press, sizeof press, keys, values, 16) != 2 ||
            keys[0] != REMOTE_BASE + 3 || values[0] != 1 ||
            keys[1] != REMOTE_CENTER || values[1] != 1 ||
            raw_parse(&held, up, sizeof up, keys, values, 16) != 2 ||
            values[0] != 0 || values[1] != 0 ||
            raw_parse(&held, ccw, sizeof ccw, keys, values, 16) != 1 ||
            keys[0] != REMOTE_DIAL_CCW || values[0] != 1 ||
            raw_parse(&held, battery, sizeof battery, keys, values, 16) != 0 ||
            key_from_name("remote_center", &code) != 0 || code != REMOTE_CENTER ||
            strcmp(key_name(REMOTE_BASE + 9), "REMOTE_BTN10") != 0 ||
            key_from_name("CTRL+REMOTE_BTN1", &code) == 0) {
            log_error("keys", "raw remote report test failed");
            return -1;
        }
    }
    if (debounce_self_test() != 0 || combo_self_test() != 0 || kbd_self_test() != 0 ||
        midi_dispatch_self_test() != 0) {
        return -1;
    }
    return 0;
}
