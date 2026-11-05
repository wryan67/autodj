#include "audio.h"

#include "log.h"

#include <alsa/asoundlib.h>

#include <string.h>
#include <time.h>

static snd_pcm_t *g_pcm;
static unsigned g_period = 1024;
static struct timespec g_xrun_log;

int audio_open(unsigned *rate)
{
    static const char *devices[] = {"default", "pipewire", "pulse", NULL};
    snd_pcm_hw_params_t *hw;
    snd_pcm_uframes_t period = 1024;
    unsigned periods = 4;
    unsigned got;
    int i;
    int err = -1;

    if (!rate || *rate == 0) {
        return -1;
    }

    for (i = 0; devices[i]; i++) {
        err = snd_pcm_open(&g_pcm, devices[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) {
            log_info("audio", "output %s", devices[i]);
            break;
        }
        log_warn("audio", "cannot open %s: %s", devices[i], snd_strerror(err));
        g_pcm = NULL;
    }
    if (!g_pcm) {
        log_error("audio", "no playback device");
        return -1;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    err = snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err == 0) {
        err = snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_channels(g_pcm, hw, 2);
    }
    got = *rate;
    if (err == 0) {
        err = snd_pcm_hw_params_set_rate_near(g_pcm, hw, &got, 0);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, 0);
    }
    if (err == 0) {
        err = snd_pcm_hw_params_set_periods_near(g_pcm, hw, &periods, 0);
    }
    if (err == 0) {
        err = snd_pcm_hw_params(g_pcm, hw);
    }
    if (err < 0) {
        log_error("audio", "cannot configure playback: %s", snd_strerror(err));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }

    g_period = (unsigned)period;
    *rate = got;
    log_info("audio", "output %u Hz stereo, period %u frames", got, g_period);
    return 0;
}

void audio_close(void)
{
    if (!g_pcm) {
        return;
    }
    snd_pcm_drop(g_pcm);
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
}

unsigned audio_period(void)
{
    return g_period;
}

int audio_write(const int16_t *interleaved, unsigned frames)
{
    unsigned off = 0;

    if (!g_pcm) {
        return -1;
    }
    while (off < frames) {
        snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, interleaved + off * 2, frames - off);
        if (n == -EAGAIN) {
            continue;
        }
        if (n < 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec != g_xrun_log.tv_sec) {
                log_warn("audio", "playback recover: %s", snd_strerror((int)n));
                g_xrun_log = now;
            }
            n = snd_pcm_recover(g_pcm, (int)n, 1);
            if (n < 0) {
                log_error("audio", "playback failed: %s", snd_strerror((int)n));
                return -1;
            }
            continue;
        }
        off += (unsigned)n;
    }
    return 0;
}

int audio_self_test(void)
{
    unsigned rate = 44100;
    int16_t silence[256];
    int rc;

    memset(silence, 0, sizeof silence);
    rc = audio_open(&rate);
    if (rc != 0) {
        return -1;
    }
    rc = audio_write(silence, 128);
    audio_close();
    return rc;
}
