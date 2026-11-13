#include "midi.h"

#include "config.h"
#include "keys.h"
#include "log.h"
#include "player.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * Raw MIDI bytes from /dev/snd/midiC<card>D<device>. Pads send Note On and
 * Note Off. Knobs send Control Change. The Akai MPD218 knobs are absolute
 * (0 to 127) in the factory presets, so a turn is the change from the last
 * value, and the knob stops sending at either end. In its "Inc/Dec 2" mode
 * a knob is relative: 1, 2, 3 turn clockwise and 127, 126, 125 turn
 * counterclockwise. An absolute knob never sends the same value twice in a
 * row, so a repeated value marks the knob as relative from then on.
 *
 * A pad bound to an effect shows whether the effect is on: autodj sends
 * Note On to light it and Note Off to darken it, on the channel the pad
 * sends on (channel 10 until the pad is first pressed). The MPD218 lights a
 * pad while it is held and darkens it on release, so the state is sent
 * again after each release.
 */

enum {
    MAX_MIDI = 8,
    MIDI_RETRY_MS = 3000,
    /* An absolute knob moves one step per message. A larger jump is a
     * relative knob changing direction, or a knob seen for the first time. */
    ABS_JUMP_MAX = 32
};

struct MidiIn {
    int fd;
    char path[32];
    char name[64];
    unsigned char status;  /* running status, 0 when none */
    unsigned char data[2];
    int have;
    int sysex;
    signed char last[128]; /* last value of each CC, -1 when unknown */
    unsigned char relative[128];
    int out;                  /* 1 when fd can also send to the device */
    unsigned char note_ch[128]; /* channel each note was last seen on */
    signed char led[128];     /* pad light as last sent or seen, -1 unknown */
};

/* Effect actions whose pads show the effect's state. */
static const struct {
    int id;
    int fx;
} g_led_fx[] = {
    {KEYID_REVERB, PLAYER_FX_REVERB},   {KEYID_PITCH, PLAYER_FX_PITCH},
    {KEYID_AUTOPAN, PLAYER_FX_AUTOPAN}, {KEYID_PHASER, PLAYER_FX_PHASER},
    {KEYID_CHORUS, PLAYER_FX_CHORUS},   {KEYID_FLANGER, PLAYER_FX_FLANGER},
};

static struct MidiIn g_in[MAX_MIDI];
static int g_n;
static int g_wanted;
static struct timespec g_retry;
static int g_last_unmapped = -1;

static void in_reset(struct MidiIn *m, int fd)
{
    memset(m, 0, sizeof *m);
    m->fd = fd;
    memset(m->last, -1, sizeof m->last);
    memset(m->note_ch, 9, sizeof m->note_ch);
    memset(m->led, -1, sizeof m->led);
}

int midi_is_knob(int code)
{
    int kind = code & MIDI_KIND_MASK;

    return code >= 0 && (kind == MIDI_CC_UP || kind == MIDI_CC_DOWN);
}

static int parse_num(const char *s, size_t len, int *out)
{
    int v = 0;
    size_t i;

    if (len == 0 || len > 3) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    if (v > 127) {
        return -1;
    }
    *out = v;
    return 0;
}

int midi_from_name(const char *name, int *code)
{
    int n = 0;
    size_t len;

    if (!name || !code) {
        return -1;
    }
    len = strlen(name);
    if (len > 5 && strncasecmp(name, "NOTE_", 5) == 0) {
        if (parse_num(name + 5, len - 5, &n) != 0) {
            return -1;
        }
        *code = MIDI_NOTE | n;
        return 0;
    }
    if (len > 3 && strncasecmp(name, "CC_", 3) == 0) {
        const char *us = strchr(name + 3, '_');

        if (!us || parse_num(name + 3, (size_t)(us - (name + 3)), &n) != 0) {
            return -1;
        }
        if (strcasecmp(us + 1, "UP") == 0) {
            *code = MIDI_CC_UP | n;
            return 0;
        }
        if (strcasecmp(us + 1, "DOWN") == 0) {
            *code = MIDI_CC_DOWN | n;
            return 0;
        }
    }
    return -1;
}

const char *midi_name(int code, char *buf, size_t n)
{
    int num = code & MIDI_NUM_MASK;

    if (!buf || n == 0) {
        return "";
    }
    switch (code < 0 ? -1 : code & MIDI_KIND_MASK) {
    case MIDI_NOTE:
        snprintf(buf, n, "NOTE_%d", num);
        break;
    case MIDI_CC_UP:
        snprintf(buf, n, "CC_%d_UP", num);
        break;
    case MIDI_CC_DOWN:
        snprintf(buf, n, "CC_%d_DOWN", num);
        break;
    default:
        buf[0] = '\0';
        break;
    }
    return buf;
}

int midi_canon(const char *name, char *dest, size_t n)
{
    char buf[AUTODJ_MIDI_LEN];
    int code = -1;
    size_t len;

    if (!dest || n == 0) {
        return -1;
    }
    if (!name) {
        dest[0] = '\0';
        return -1;
    }
    while (*name && isspace((unsigned char)*name)) {
        name++;
    }
    len = strlen(name);
    while (len > 0 && isspace((unsigned char)name[len - 1])) {
        len--;
    }
    if (len == 0 || len >= sizeof buf) {
        dest[0] = '\0';
        return -1;
    }
    memcpy(buf, name, len);
    buf[len] = '\0';
    if (midi_from_name(buf, &code) != 0) {
        return -1;
    }
    midi_name(code, buf, sizeof buf);
    if (strlen(buf) >= n) {
        dest[0] = '\0';
        return -1;
    }
    snprintf(dest, n, "%s", buf);
    return 0;
}

/* Turn of a knob from one CC value. Returns the signed number of steps, or
 * 0 when the direction is not known yet. */
static int knob_steps(struct MidiIn *m, int cc, int value)
{
    int last = m->last[cc];
    int steps;

    m->last[cc] = (signed char)value;
    if (!m->relative[cc] && last >= 0 && value == last) {
        m->relative[cc] = 1;
    }
    if (m->relative[cc]) {
        if (value == 0 || value == 64) {
            return 0;
        }
        return value < 64 ? value : value - 128;
    }
    if (last < 0) {
        return 0;
    }
    steps = value - last;
    if (steps > ABS_JUMP_MAX || steps < -ABS_JUMP_MAX) {
        return 0;
    }
    return steps;
}

/* Feed one byte. Returns 1 with *code and *steps set for a pad press or a
 * knob turn. *steps is 1 for a pad and the size of the turn for a knob. */
static int midi_feed(struct MidiIn *m, unsigned char b, int *code, int *steps)
{
    unsigned char kind;
    int need;

    if (b >= 0xf8) {
        return 0; /* real-time bytes can come between any two bytes */
    }
    if (b == 0xf0) {
        m->sysex = 1;
        m->status = 0;
        return 0;
    }
    if (b >= 0x80) {
        m->sysex = 0;
        m->have = 0;
        m->status = b < 0xf0 ? b : 0;
        return 0;
    }
    if (m->sysex || !m->status) {
        return 0;
    }
    kind = m->status & 0xf0;
    need = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
    m->data[m->have++] = b;
    if (m->have < need) {
        return 0;
    }
    m->have = 0;
    if (kind == 0x80 || kind == 0x90) {
        /* The pad lights while held and goes dark on release. */
        m->note_ch[m->data[0]] = m->status & 0x0f;
        m->led[m->data[0]] = kind == 0x90 && m->data[1] > 0;
    }
    if (kind == 0x90 && m->data[1] > 0) {
        *code = MIDI_NOTE | m->data[0];
        *steps = 1;
        return 1;
    }
    if (kind == 0xb0) {
        int turn = knob_steps(m, m->data[0], m->data[1]);

        if (turn == 0) {
            return 0;
        }
        *code = (turn > 0 ? MIDI_CC_UP : MIDI_CC_DOWN) | m->data[0];
        *steps = turn > 0 ? turn : -turn;
        return 1;
    }
    return 0;
}

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *fp = fopen(path, "r");

    if (!fp) {
        return -1;
    }
    if (!fgets(buf, (int)n, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/* Name of a hardware MIDI port with an input, or -1 for a virtual port
 * (snd-virmidi) or a port that only sends. */
static int port_name(int card, int dev, char *name, size_t n, int *output)
{
    char path[96];
    char line[128];
    FILE *fp;
    int input = 0;

    *output = 0;

    snprintf(path, sizeof path, "/proc/asound/card%d/midi%d", card, dev);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(line, sizeof line, fp)) {
        fclose(fp);
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';
    snprintf(name, n, "%.60s", line);
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, "Input", 5) == 0) {
            input = 1;
        }
        if (strncmp(line, "Output", 6) == 0) {
            *output = 1;
        }
    }
    fclose(fp);
    if (!input || strstr(name, "Virtual Raw MIDI")) {
        return -1;
    }
    snprintf(path, sizeof path, "/proc/asound/card%d/id", card);
    if (read_line(path, line, sizeof line) == 0 && strcmp(line, "VirMIDI") == 0) {
        return -1;
    }
    return 0;
}

static int path_open(const struct MidiIn *list, int count, const char *path)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Open MIDI inputs not already in list[0..*count). quiet leaves out logs.
 * *busy counts the inputs another program holds. */
static void scan(struct MidiIn *list, int *count, int max, int quiet, int *busy)
{
    DIR *dir = opendir("/dev/snd");
    struct dirent *de;

    if (!dir) {
        return;
    }
    while (*count < max && (de = readdir(dir)) != NULL) {
        char path[32];
        char name[64];
        int card;
        int dev;
        int fd = -1;
        int output = 0;
        char tail;

        if (sscanf(de->d_name, "midiC%dD%d%c", &card, &dev, &tail) != 2) {
            continue;
        }
        snprintf(path, sizeof path, "/dev/snd/%.20s", de->d_name);
        if (path_open(list, *count, path) ||
            port_name(card, dev, name, sizeof name, &output) != 0) {
            continue;
        }
        /* Playback also sends to the pads to light them. The editor only reads. */
        if (output && list == g_in) {
            fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        }
        if (fd >= 0) {
            output = 1;
        } else {
            output = 0;
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (fd < 0) {
            if (busy && errno == EBUSY) {
                (*busy)++;
            }
            if (!quiet) {
                log_warn("midi", "cannot open %s (%s): %s", name, path, strerror(errno));
            }
            continue;
        }
        in_reset(&list[*count], fd);
        list[*count].out = output;
        snprintf(list[*count].path, sizeof list[*count].path, "%s", path);
        snprintf(list[*count].name, sizeof list[*count].name, "%s", name);
        (*count)++;
        if (!quiet) {
            log_info("midi", "%s (%s) opened", name, path);
        }
    }
    closedir(dir);
}

int midi_open(void)
{
    g_n = 0;
    g_wanted = 1;
    g_last_unmapped = -1;
    clock_gettime(CLOCK_MONOTONIC, &g_retry);
    scan(g_in, &g_n, MAX_MIDI, 0, NULL);
    if (g_n == 0) {
        log_info("midi", "no MIDI controller found");
    }
    return g_n;
}

static void send_led(struct MidiIn *m, int note, int on)
{
    unsigned char msg[3];

    msg[0] = (unsigned char)((on ? 0x90 : 0x80) | m->note_ch[note]);
    msg[1] = (unsigned char)note;
    msg[2] = on ? 127 : 0;
    if (write(m->fd, msg, sizeof msg) == (ssize_t)sizeof msg) {
        m->led[note] = (signed char)on;
    }
}

/* Light each effect pad that is on and darken the rest. Only changes are sent. */
static void leds_sync(struct MidiIn *m, const Config *cfg)
{
    size_t i;

    if (!m->out) {
        return;
    }
    for (i = 0; i < sizeof g_led_fx / sizeof g_led_fx[0]; i++) {
        int code = cfg->midi_code[g_led_fx[i].id];
        int note = code & MIDI_NUM_MASK;
        int want;

        if (code < 0 || (code & MIDI_KIND_MASK) != MIDI_NOTE) {
            continue;
        }
        want = player_fx_enabled(g_led_fx[i].fx);
        if (m->led[note] != want) {
            send_led(m, note, want);
        }
    }
}

static void drop(int i)
{
    close(g_in[i].fd);
    memmove(&g_in[i], &g_in[i + 1], (size_t)(g_n - i - 1) * sizeof g_in[0]);
    g_n--;
}

void midi_close(void)
{
    int i;

    /* Leave no pad lit after autodj stops. */
    for (i = 0; i < g_n; i++) {
        int note;

        for (note = 0; note < 128 && g_in[i].out; note++) {
            if (g_in[i].led[note] == 1) {
                send_led(&g_in[i], note, 0);
            }
        }
    }
    while (g_n > 0) {
        drop(g_n - 1);
    }
    g_wanted = 0;
}

int midi_fds(int *fds, int max)
{
    int i;

    for (i = 0; i < g_n && i < max; i++) {
        fds[i] = g_in[i].fd;
    }
    return i;
}

static void run(const Config *cfg, int code, int steps)
{
    struct timeval when;

    gettimeofday(&when, NULL);
    if (keys_midi_dispatch(cfg, code, steps, &when) == 0 && code != g_last_unmapped) {
        char name[AUTODJ_MIDI_LEN];

        /* A knob sends many messages, so log each control once in a row. */
        log_info("midi", "%s is not mapped", midi_name(code, name, sizeof name));
        g_last_unmapped = code;
    }
}

void midi_poll(const Config *cfg)
{
    struct timespec now;
    int i;

    if (!cfg) {
        return;
    }
    for (i = 0; i < g_n; i++) {
        unsigned char buf[256];
        ssize_t n;

        while ((n = read(g_in[i].fd, buf, sizeof buf)) > 0) {
            ssize_t k;

            for (k = 0; k < n; k++) {
                int code;
                int steps;

                if (midi_feed(&g_in[i], buf[k], &code, &steps)) {
                    run(cfg, code, steps);
                }
            }
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            log_warn("midi", "%s was disconnected", g_in[i].name);
            drop(i);
            i--;
            continue;
        }
        leds_sync(&g_in[i], cfg);
    }
    if (!g_wanted || g_n >= MAX_MIDI) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - g_retry.tv_sec) * 1000 + (now.tv_nsec - g_retry.tv_nsec) / 1000000 >=
        MIDI_RETRY_MS) {
        int before = g_n;

        g_retry = now;
        /* Quiet, so a device that cannot be opened is not logged every time. */
        scan(g_in, &g_n, MAX_MIDI, 1, NULL);
        for (i = before; i < g_n; i++) {
            log_info("midi", "%s (%s) opened", g_in[i].name, g_in[i].path);
        }
    }
}

/* Parser state for capture fds, by slot. */
static struct MidiIn g_cap[MAX_MIDI];
static int g_ncap;

int midi_open_capture(int *fds, int max, char *names, size_t names_n, int *busy)
{
    int i;
    size_t used = 0;

    g_ncap = 0;
    if (busy) {
        *busy = 0;
    }
    if (names && names_n) {
        names[0] = '\0';
    }
    if (!fds || max <= 0) {
        return 0;
    }
    scan(g_cap, &g_ncap, max < MAX_MIDI ? max : MAX_MIDI, 1, busy);
    for (i = 0; i < g_ncap; i++) {
        fds[i] = g_cap[i].fd;
        if (names && names_n > used) {
            int w = snprintf(names + used, names_n - used, "%s%s", i ? ", " : "", g_cap[i].name);

            if (w > 0) {
                used += (size_t)w < names_n - used ? (size_t)w : names_n - used - 1;
            }
        }
    }
    return g_ncap;
}

void midi_close_capture(int *fds, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (fds && fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
    g_ncap = 0;
}

int midi_read_press(int fd)
{
    struct MidiIn *m = NULL;
    unsigned char buf[256];
    ssize_t n;
    int found = -1;
    int i;

    for (i = 0; i < g_ncap; i++) {
        if (g_cap[i].fd == fd) {
            m = &g_cap[i];
        }
    }
    if (!m) {
        return -1;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ssize_t k;

        for (k = 0; k < n; k++) {
            int code;
            int steps;

            if (midi_feed(m, buf[k], &code, &steps) && found < 0) {
                found = code;
            }
        }
    }
    if (found < 0 && (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))) {
        close(fd);
        memmove(m, m + 1, (size_t)(g_ncap - (m - g_cap) - 1) * sizeof *m);
        g_ncap--;
        return -2;
    }
    return found;
}

static int feed_all(struct MidiIn *m, const unsigned char *bytes, int n, int *codes, int *steps,
                    int max)
{
    int got = 0;
    int i;

    for (i = 0; i < n; i++) {
        int code;
        int s;

        if (midi_feed(m, bytes[i], &code, &s) && got < max) {
            codes[got] = code;
            steps[got++] = s;
        }
    }
    return got;
}

/* An effect pad is lit while the effect is on, and lit again after the
 * pad's own release darkens it. */
static int leds_self_test(void)
{
    static const unsigned char press[] = {0x99, 36, 127};
    static const unsigned char release[] = {0x89, 36, 0};
    struct MidiIn m;
    Config cfg;
    unsigned char got[16];
    int pipefd[2];
    int code;
    int steps;
    int rc = -1;
    int id;
    size_t i;

    if (pipe(pipefd) != 0) {
        log_error("midi", "cannot create a pad light pipe");
        return -1;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    memset(&cfg, 0, sizeof cfg);
    for (id = 0; id < KEYID_COUNT; id++) {
        cfg.midi_code[id] = -1;
    }
    cfg.midi_code[KEYID_REVERB] = MIDI_NOTE | 36;
    cfg.midi_code[KEYID_CHORUS] = MIDI_CC_UP | 3;
    player_set_fx(NULL);
    in_reset(&m, pipefd[1]);
    m.out = 1;

    /* At the start the pad is darkened once. */
    leds_sync(&m, &cfg);
    leds_sync(&m, &cfg);
    if (read(pipefd[0], got, sizeof got) != 3 || got[0] != 0x89 || got[1] != 36) {
        log_error("midi", "the reverb pad was not darkened at the start");
        goto out;
    }
    /* Press: the pad lights itself and the effect turns on, so nothing is sent. */
    for (i = 0; i < sizeof press; i++) {
        midi_feed(&m, press[i], &code, &steps);
    }
    player_fx_toggle(PLAYER_FX_REVERB);
    leds_sync(&m, &cfg);
    if (read(pipefd[0], got, sizeof got) > 0) {
        log_error("midi", "a held pad was sent its light again");
        goto out;
    }
    /* Release darkens it, so it is lit again. */
    for (i = 0; i < sizeof release; i++) {
        midi_feed(&m, release[i], &code, &steps);
    }
    leds_sync(&m, &cfg);
    if (read(pipefd[0], got, sizeof got) != 3 || got[0] != 0x99 || got[1] != 36 ||
        got[2] != 127) {
        log_error("midi", "the reverb pad was not lit after its release");
        goto out;
    }
    player_fx_all_off();
    leds_sync(&m, &cfg);
    if (read(pipefd[0], got, sizeof got) != 3 || got[0] != 0x89 || got[1] != 36) {
        log_error("midi", "the reverb pad stayed lit with every effect off");
        goto out;
    }
    rc = 0;
out:
    player_set_fx(NULL);
    close(pipefd[0]);
    close(pipefd[1]);
    return rc;
}

int midi_self_test(void)
{
    struct MidiIn m;
    int codes[16];
    int steps[16];
    int code = -1;
    char buf[AUTODJ_MIDI_LEN];

    if (midi_from_name("NOTE_36", &code) != 0 || code != (MIDI_NOTE | 36) ||
        midi_from_name("cc_3_down", &code) != 0 || code != (MIDI_CC_DOWN | 3) ||
        midi_from_name("CC_127_UP", &code) != 0 || code != (MIDI_CC_UP | 127) ||
        midi_from_name("NOTE_128", &code) == 0 || midi_from_name("CC_3", &code) == 0 ||
        midi_from_name("KEY_A", &code) == 0 || midi_from_name("NOTE_", &code) == 0 ||
        midi_canon(" note_5 ", buf, sizeof buf) != 0 || strcmp(buf, "NOTE_5") != 0 ||
        strcmp(midi_name(MIDI_CC_UP | 9, buf, sizeof buf), "CC_9_UP") != 0) {
        log_error("midi", "MIDI names did not parse");
        return -1;
    }

    /* MPD218 pad 1 on channel 10: press, channel pressure, release. A
     * real-time byte in the middle of a message is skipped. */
    {
        static const unsigned char pad[] = {0x99, 36, 0xfe, 127, 0xd9, 40, 0x89, 36, 0,
                                            0x99, 37, 100, 37, 0};

        in_reset(&m, -1);
        if (feed_all(&m, pad, (int)sizeof pad, codes, steps, 16) != 2 ||
            codes[0] != (MIDI_NOTE | 36) || codes[1] != (MIDI_NOTE | 37) || steps[0] != 1) {
            log_error("midi", "pad presses did not decode");
            return -1;
        }
    }
    /* An absolute knob: the first value only sets the position. */
    {
        static const unsigned char knob[] = {0xb0, 3, 64, 3, 65, 3, 67, 3, 66, 0xf0, 1, 2, 0xf7,
                                             0xb0, 3, 10};

        in_reset(&m, -1);
        if (feed_all(&m, knob, (int)sizeof knob, codes, steps, 16) != 3 ||
            codes[0] != (MIDI_CC_UP | 3) || steps[0] != 1 ||
            codes[1] != (MIDI_CC_UP | 3) || steps[1] != 2 ||
            codes[2] != (MIDI_CC_DOWN | 3) || steps[2] != 1) {
            log_error("midi", "absolute knob did not decode");
            return -1;
        }
    }
    /* Inc/Dec 2: a repeated value makes the knob relative. */
    {
        static const unsigned char knob[] = {0xb0, 9, 1, 9, 1, 9, 2, 9, 127, 9, 125};

        in_reset(&m, -1);
        if (feed_all(&m, knob, (int)sizeof knob, codes, steps, 16) != 4 ||
            codes[0] != (MIDI_CC_UP | 9) || steps[0] != 1 ||
            codes[1] != (MIDI_CC_UP | 9) || steps[1] != 2 ||
            codes[2] != (MIDI_CC_DOWN | 9) || steps[2] != 1 ||
            codes[3] != (MIDI_CC_DOWN | 9) || steps[3] != 3) {
            log_error("midi", "relative knob did not decode");
            return -1;
        }
    }
    return leds_self_test();
}
