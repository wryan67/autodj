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
        if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
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
    return 0;
}
