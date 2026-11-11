#include "audio.h"
#include "cfgedit.h"
#include "config.h"
#include "decode.h"
#include "keys.h"
#include "log.h"
#include "player.h"
#include "playlist.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_sig;

static void on_signal(int signo)
{
    (void)signo;
    g_sig = 1;
}

static void default_config_path(char *dest, size_t n)
{
    const char *home = getenv("HOME");

    if (!home || !home[0]) {
        home = "/home/pi";
    }
    snprintf(dest, n, "%s/.config/autodj/master.cfg", home);
}

static int run_checks(const Config *cfg, Playlist *pl)
{
    char first[4096];
    int failed = 0;

    if (keys_self_test() != 0) {
        failed = 1;
    }
    if (config_self_test() != 0) {
        failed = 1;
    }
    if (cfgedit_self_test() != 0) {
        failed = 1;
    }
    if (trim_self_test() != 0) {
        failed = 1;
    }
    if (loudness_self_test() != 0) {
        failed = 1;
    }
    if (player_self_test() != 0) {
        failed = 1;
    }
    if (fx_self_test() != 0) {
        failed = 1;
    }
    if (playlist_next(pl, first, sizeof first) != 0 || decode_self_test(first) != 0) {
        failed = 1;
    }
    if (audio_self_test() != 0) {
        failed = 1;
    }
    if (keys_open() != 0) {
        failed = 1;
    } else {
        keys_close();
    }
    if (failed) {
        log_error("autodj", "self-check failed");
        return 1;
    }
    log_info("autodj", "self-check passed (%zu tracks, overlap %ds)",
             playlist_count(pl), cfg->overlap_sec);
    return 0;
}

static void log_keyboard_bindings(const Config *cfg)
{
    static const char *labels[] = {
        "pause/resume", "skip", "previous", "rewind 0.5s", "rewind 1s",
        "rewind 1.5s", "dial forward", "dial back", "dial mode",
        "reverb", "chipmunk", "autopan", "phaser", "chorus", "flanger",
        "effects off"
    };
    const char *vals[] = {
        cfg->kbd_pause, cfg->kbd_skip, cfg->kbd_prev, cfg->kbd_rew05,
        cfg->kbd_rew10, cfg->kbd_rew15, cfg->kbd_dial_fwd, cfg->kbd_dial_back,
        cfg->kbd_dial_mode, cfg->kbd_reverb, cfg->kbd_pitch, cfg->kbd_autopan,
        cfg->kbd_phaser, cfg->kbd_chorus, cfg->kbd_flanger, cfg->kbd_effects_off
    };
    char buf[512];
    size_t used = 0;
    int i;

    buf[0] = '\0';
    for (i = 0; i < (int)(sizeof vals / sizeof vals[0]); i++) {
        int n;

        if (!vals[i] || !vals[i][0]) {
            continue;
        }
        n = snprintf(buf + used, sizeof buf - used, "%s%s %s",
                     used ? ", " : "", labels[i], vals[i]);
        if (n < 0 || (size_t)n >= sizeof buf - used) {
            break;
        }
        used += (size_t)n;
    }
    if (used) {
        log_info("autodj", "keyboard %s", buf);
    }
}

static void remember_volume(const char *path, Config *cfg)
{
    int volume = player_volume_percent();

    if (volume == cfg->volume) {
        return;
    }
    if (config_save_volume(path, volume) != 0) {
        return;
    }
    cfg->volume = volume;
    cfg->volume_set = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: autodj [--check] [config]\n"
            "       autodj --config [config]\n"
            "       autodj --calibrate [config]\n"
            "  config defaults to ~/.config/autodj/master.cfg\n");
}

int main(int argc, char **argv)
{
    char path_buf[4096];
    const char *path = NULL;
    int check = 0;
    int edit = 0;
    int calibrate = 0;
    int i;
    Config cfg;
    Playlist *pl;
    PlayerSettings settings;
    struct sigaction sa;
    unsigned rate;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[i], "--check") == 0) {
            check = 1;
            continue;
        }
        if (strcmp(argv[i], "--config") == 0) {
            edit = 1;
            continue;
        }
        if (strcmp(argv[i], "--calibrate") == 0) {
            calibrate = 1;
            continue;
        }
        if (path) {
            usage();
            return 2;
        }
        path = argv[i];
    }
    if (edit + check + calibrate > 1) {
        usage();
        return 2;
    }
    if (!path) {
        default_config_path(path_buf, sizeof path_buf);
        path = path_buf;
    }
    if (edit) {
        return config_edit(path) == 0 ? 0 : 1;
    }
    if (calibrate) {
        return dial_calibrate(path) == 0 ? 0 : 1;
    }

    log_info("autodj", "starting");
    log_info("autodj", "config %s", path);
    if (config_load(path, &cfg) != 0) {
        return 1;
    }
    log_info("autodj", "folder %s", cfg.folder);
    log_info("autodj", "overlap %ds, trim silence %s, pause %ds, resume %ds, volume %d%%",
             cfg.overlap_sec, cfg.trim_silence ? "on" : "off", cfg.pause_sec, cfg.resume_sec,
             cfg.volume);
    if (cfg.loudness) {
        log_info("autodj", "loudness on, target %d LUFS", cfg.loudness_lufs);
    } else {
        log_info("autodj", "loudness off");
    }
    if (!cfg.volume_set) {
        if (config_save_volume(path, cfg.volume) == 0) {
            cfg.volume_set = 1;
        }
    }
    player_set_volume_percent(cfg.volume);
    player_set_fx(&cfg.fx);
    log_info("autodj", "keys pause/resume %s, skip %s, previous %s",
             cfg.key_pause_name[0] ? cfg.key_pause_name : "(none)",
             cfg.key_skip_name[0] ? cfg.key_skip_name : "(none)",
             cfg.key_prev_name[0] ? cfg.key_prev_name : "(none)");
    log_info("autodj", "keys reverb %s, chipmunk %s, autopan %s, phaser %s",
             cfg.key_reverb_name[0] ? cfg.key_reverb_name : "(none)",
             cfg.key_pitch_name[0] ? cfg.key_pitch_name : "(none)",
             cfg.key_autopan_name[0] ? cfg.key_autopan_name : "(none)",
             cfg.key_phaser_name[0] ? cfg.key_phaser_name : "(none)");
    log_info("autodj", "keys chorus %s, flanger %s, effects off %s",
             cfg.key_chorus_name[0] ? cfg.key_chorus_name : "(none)",
             cfg.key_flanger_name[0] ? cfg.key_flanger_name : "(none)",
             cfg.key_effects_off_name[0] ? cfg.key_effects_off_name : "(none)");
    log_info("autodj", "effects off");
    log_info("autodj", "keys rewind 0.5s %s, rewind 1s %s, rewind 1.5s %s",
             cfg.key_rew05_name[0] ? cfg.key_rew05_name : "(none)",
             cfg.key_rew10_name[0] ? cfg.key_rew10_name : "(none)",
             cfg.key_rew15_name[0] ? cfg.key_rew15_name : "(none)");
    log_info("autodj", "keys dial forward %s, dial back %s, dial mode %s",
             cfg.key_dial_fwd_name[0] ? cfg.key_dial_fwd_name : "(none)",
             cfg.key_dial_back_name[0] ? cfg.key_dial_back_name : "(none)",
             cfg.key_dial_mode_name[0] ? cfg.key_dial_mode_name : "(none)");
    log_keyboard_bindings(&cfg);
    log_info("autodj", "jog %gs per turn, %d clicks per turn, max speed %gx, spin-up %gs, grab %gs, smoothing %gs",
             cfg.jog_turn_sec, cfg.jog_clicks, cfg.jog_max_speed, cfg.jog_spinup_sec,
             cfg.jog_grab_sec, cfg.jog_smooth_sec);
    {
        JogSettings jog;

        jog.turn_sec = cfg.jog_turn_sec;
        jog.clicks = cfg.jog_clicks;
        jog.max_speed = cfg.jog_max_speed;
        jog.spinup_sec = cfg.jog_spinup_sec;
        jog.grab_sec = cfg.jog_grab_sec;
        jog.smooth_sec = cfg.jog_smooth_sec;
        player_set_jog(&jog);
    }

    pl = playlist_load(cfg.folder, cfg.shuffle);
    if (!pl) {
        return 1;
    }
    if (decode_init() != 0) {
        playlist_free(pl);
        return 1;
    }

    if (check) {
        int rc = run_checks(&cfg, pl);
        decode_shutdown();
        playlist_free(pl);
        return rc;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rate = 44100;
    if (audio_open(&rate) != 0) {
        decode_shutdown();
        playlist_free(pl);
        return 1;
    }
    keys_open();
    keys_set_dial_volume(cfg.dial_volume);

    settings.overlap_sec = cfg.overlap_sec;
    settings.pause_sec = cfg.pause_sec;
    settings.resume_sec = cfg.resume_sec;
    settings.trim = cfg.trim_silence;
    settings.loudness = cfg.loudness;
    settings.loudness_lufs = cfg.loudness_lufs;
    settings.rate = rate;
    keys_kbd_open();
    if (player_start(&settings, pl) != 0) {
        keys_kbd_close();
        keys_close();
        audio_close();
        decode_shutdown();
        playlist_free(pl);
        return 1;
    }

    while (!g_sig && player_alive()) {
        struct pollfd pfd[9];
        int fds[8];
        int nfds;
        int extra = 0;
        int kfd;
        int i;
        int pr;

        nfds = keys_fds(fds, 8);
        for (i = 0; i < nfds; i++) {
            pfd[i].fd = fds[i];
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        kfd = keys_kbd_fd();
        if (kfd >= 0) {
            pfd[nfds].fd = kfd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            extra = 1;
        }
        pr = poll(nfds + extra ? pfd : NULL, (nfds_t)(nfds + extra), keys_wait_ms(200));
        if (pr < 0 && errno == EINTR) {
            continue;
        }
        /* Also on timeout, for held remote buttons and reconnects. */
        keys_poll(&cfg);
        keys_kbd_poll(&cfg);
        remember_volume(path, &cfg);
    }

    remember_volume(path, &cfg);
    log_info("autodj", "stopping");
    player_stop();
    keys_kbd_close();
    keys_close();
    audio_close();
    decode_shutdown();
    playlist_free(pl);
    log_info("autodj", "stopped");
    return 0;
}
