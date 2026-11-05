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

static int g_fd = -1;
static struct timespec g_vol_time;

int key_from_name(const char *name, int *code)
{
    size_t i;

    for (i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        if (strcmp(g_keys[i].name, name) == 0) {
            *code = g_keys[i].code;
            return 0;
        }
    }
    return -1;
}

const char *key_name(int code)
{
    size_t i;

    for (i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        if (g_keys[i].code == code) {
            return g_keys[i].name;
        }
    }
    return NULL;
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
    int found = -1;

    if (fd < 0) {
        return -1;
    }
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
        if (found >= 0 || ev.type != EV_KEY || ev.value != 1) {
            continue;
        }
        if (ev.code == KEY_NUMLOCK || ev.code == KEY_RESERVED) {
            continue;
        }
        found = (int)ev.code;
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
        pad = strcmp(name, "USB Keypad") == 0 && bit_set(bits, KEY_KP0);
        board = bit_set(bits, KEY_A) || bit_set(bits, KEY_KP0);
        if (!pad && !board) {
            close(fd);
            continue;
        }
        drain_fd(fd);
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
        *keypad = npads > 0;
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

    for (i = 0; i < 32; i++) {
        char path[64];
        char name[256];
        unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        memset(name, 0, sizeof name);
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) < 0 || strcmp(name, "USB Keypad") != 0) {
            close(fd);
            continue;
        }
        memset(bits, 0, sizeof bits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 || !bit_set(bits, KEY_KP0)) {
            close(fd);
            continue;
        }
        /* EVIOCGRAB takes the integer in the argument itself, not a pointer.
         * A non-zero value grabs; zero releases. */
        if (keys_grab(fd) < 0) {
            log_warn("keys", "could not grab %s: %s", path, strerror(errno));
        } else {
            log_info("keys", "keypad %s grabbed", path);
        }
        g_fd = fd;
        return 0;
    }
    log_warn("keys", "USB keypad not found, playback will continue without keys");
    return -1;
}

void keys_close(void)
{
    if (g_fd < 0) {
        return;
    }
    ioctl(g_fd, EVIOCGRAB, (void *)0);
    close(g_fd);
    g_fd = -1;
}

int keys_fd(void)
{
    return g_fd;
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

void keys_poll(const struct Config *cfg)
{
    struct input_event ev;

    if (g_fd < 0 || !cfg) {
        return;
    }
    while (read(g_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        const char *name;

        if (ev.type != EV_KEY) {
            continue;
        }
        if (ev.code == cfg->key_pause && ev.value == 1) {
            player_toggle_pause();
            continue;
        }
        if (ev.code == cfg->key_skip && ev.value == 1) {
            player_skip();
            continue;
        }
        if ((ev.code == cfg->key_vol_up || ev.code == cfg->key_vol_dn) &&
            (ev.value == 1 || ev.value == 2)) {
            if (volume_due()) {
                player_volume_step(ev.code == cfg->key_vol_up ? 1 : -1);
            }
            continue;
        }
        if (ev.value == 1) {
            /* USB keypads emit NumLock beside every key. It is not a binding. */
            if (ev.code == KEY_NUMLOCK) {
                continue;
            }
            name = key_name(ev.code);
            log_info("keys", "key %s is not mapped", name ? name : "unknown");
        }
    }
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

            memset(name, 0, sizeof name);
            if (ioctl(fds[i], EVIOCGNAME(sizeof name), name) < 0) {
                log_error("keys", "cannot read capture device name");
                keys_close_capture(fds, n);
                return -1;
            }
            if (strcmp(name, "USB Keypad") == 0) {
                if (i != 0) {
                    log_error("keys", "keypad was not the first capture device");
                    keys_close_capture(fds, n);
                    return -1;
                }
                pads++;
            }
        }
        if (keypad != (pads > 0)) {
            log_error("keys", "keypad flag does not match the opened devices");
            keys_close_capture(fds, n);
            return -1;
        }
        if (pads > 1) {
            log_error("keys", "opened more than one USB Keypad");
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
        code = keys_read_press(pipefd[0]);
        close(pipefd[0]);
        close(pipefd[1]);
        if (code != KEY_A) {
            log_error("keys", "key press read returned %d", code);
            return -1;
        }
    }
    return 0;
}
