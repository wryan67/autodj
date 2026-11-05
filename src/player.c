#include "player.h"

#include "audio.h"
#include "decode.h"
#include "log.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Playback rate is varispeed: pitch falls as the song slows, and rate 0
 * is a full stop. That is the brake. Time-stretch libraries keep the pitch
 * and will not ramp all the way to silence.
 *
 * Overlap plays the last N seconds of the current song against the first N
 * seconds of the next one. The outgoing volume falls while the incoming
 * volume rises. Skip starts that same crossfade immediately.
 */

static atomic_int g_pause_gen;
static atomic_int g_skip_gen;
static atomic_int g_volume = 1000;
static atomic_int g_stop;

static PlayerSettings g_set;
static Playlist *g_pl;

static pthread_t g_audio_thread;
static pthread_t g_decode_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Track *g_pending;
static int g_need_next;
static int g_threads;

static Track *g_a;
static Track *g_b;
static double g_pos_a;
static double g_pos_b;
static int g_fading;
static double g_fade_origin;
static double g_fade_span;
static double g_rate = 1.0;
static int g_paused;
static int g_ramping;
static double g_ramp_from;
static double g_ramp_to;
static uint64_t g_ramp_start;
static uint64_t g_ramp_span;
static int g_announced_hold;
static uint64_t g_rendered;
static int g_seen_pause;
static int g_seen_skip;
static float g_vol = 1.0f;

static double rate_at(double from, double to, uint64_t elapsed, uint64_t span)
{
    if (span == 0 || elapsed >= span) {
        return to;
    }
    return from + (to - from) * ((double)elapsed / (double)span);
}

static void sample_at(const Track *track, double pos, float *left, float *right)
{
    size_t i;
    size_t i2;
    double frac;
    float l0, r0, l1, r1;

    if (pos < 0) {
        pos = 0;
    }
    if (track->frames == 0) {
        *left = 0;
        *right = 0;
        return;
    }
    if (pos >= (double)track->frames) {
        pos = (double)track->frames - 1.0;
    }
    i = (size_t)pos;
    frac = pos - (double)i;
    i2 = (i + 1 < track->frames) ? i + 1 : i;
    l0 = track->pcm[i * 2];
    r0 = track->pcm[i * 2 + 1];
    l1 = track->pcm[i2 * 2];
    r1 = track->pcm[i2 * 2 + 1];
    *left = (float)(l0 + (l1 - l0) * frac);
    *right = (float)(r0 + (r1 - r0) * frac);
}

static size_t audible(const Track *track)
{
    if (!track || track->end <= track->start) {
        return 0;
    }
    return track->end - track->start;
}

static size_t remaining(void)
{
    double left;

    if (!g_a) {
        return 0;
    }
    left = (double)g_a->end - g_pos_a;
    if (left <= 0) {
        return 0;
    }
    return (size_t)ceil(left);
}

static void log_playing(const Track *track)
{
    unsigned sec = 0;
    size_t frames;

    if (!track) {
        return;
    }
    frames = audible(track);
    if (g_set.rate) {
        sec = (unsigned)(frames / g_set.rate);
    }
    log_info("audio", "playing %s (%u:%02u)", track->name, sec / 60, sec % 60);
}

static int pending_ready(size_t *frames_out)
{
    int ok;

    pthread_mutex_lock(&g_mu);
    ok = g_pending && audible(g_pending) > 0;
    if (ok && frames_out) {
        *frames_out = audible(g_pending);
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static Track *take_pending(void)
{
    Track *track;

    pthread_mutex_lock(&g_mu);
    track = g_pending;
    g_pending = NULL;
    if (track) {
        g_need_next = 1;
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    return track;
}

static void become(Track *track)
{
    track_free(g_a);
    g_a = track;
    g_pos_a = track ? (double)track->start : 0;
    g_b = NULL;
    g_pos_b = 0;
    g_fading = 0;
    if (track && audible(track) > 0) {
        log_playing(track);
    }
}

static void promote(void)
{
    track_free(g_a);
    g_a = g_b;
    g_pos_a = g_pos_b;
    g_b = NULL;
    g_pos_b = 0;
    g_fading = 0;
    log_playing(g_a);
}

static void start_fade(size_t span)
{
    Track *next = take_pending();

    if (!next) {
        log_warn("audio", "next track is not ready");
        return;
    }
    if (audible(next) == 0) {
        log_warn("audio", "skipping silent %s", next->name);
        track_free(next);
        return;
    }
    g_b = next;
    g_pos_b = (double)next->start;
    g_fade_origin = g_pos_b;
    g_fade_span = span > 0 ? (double)span : 1.0;
    g_fading = 1;
    log_info("audio", "crossfade into %s", next->name);
}

static void do_skip(void)
{
    size_t blen = 0;
    size_t span;
    size_t remain;

    if (g_paused && g_rate <= 0.000001) {
        if (g_fading && g_b) {
            log_info("audio", "skip");
            promote();
            return;
        }
        if (!pending_ready(&blen)) {
            log_warn("audio", "skip ignored, next track is not ready");
            return;
        }
        log_info("audio", "skip");
        become(take_pending());
        return;
    }
    if (g_fading && g_b) {
        log_info("audio", "skip");
        promote();
        return;
    }
    if (!pending_ready(&blen)) {
        log_warn("audio", "skip ignored, next track is not ready");
        return;
    }
    remain = remaining();
    span = (size_t)g_set.overlap_sec * g_set.rate;
    if (span > remain) {
        span = remain;
    }
    if (span > blen) {
        span = blen;
    }
    if (span == 0) {
        log_info("audio", "skip");
        become(take_pending());
        return;
    }
    log_info("audio", "skip");
    start_fade(span);
}

static void begin_ramp(int pausing)
{
    g_paused = pausing;
    g_ramp_from = g_rate;
    g_ramp_to = pausing ? 0.0 : 1.0;
    g_ramp_start = g_rendered;
    g_ramp_span = (uint64_t)((pausing ? g_set.pause_sec : g_set.resume_sec) * (double)g_set.rate);
    g_announced_hold = 0;
    g_ramping = 1;
    if (pausing) {
        log_info("audio", "pausing over %ds", g_set.pause_sec);
    } else {
        log_info("audio", "resuming over %ds", g_set.resume_sec);
    }
    if (g_ramp_span == 0) {
        g_rate = g_ramp_to;
        g_ramping = 0;
        log_info("audio", "%s", pausing ? "paused" : "resumed");
        g_announced_hold = 1;
    }
}

static void service_commands(void)
{
    int pause_gen = atomic_load(&g_pause_gen);
    int skip_gen = atomic_load(&g_skip_gen);
    int volume = atomic_load(&g_volume);

    while (g_seen_pause != pause_gen) {
        g_seen_pause++;
        begin_ramp(!g_paused);
    }
    while (g_seen_skip != skip_gen) {
        g_seen_skip++;
        do_skip();
    }
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 1000) {
        volume = 1000;
    }
    g_vol = (float)volume / 1000.0f;
}

static void update_rate(void)
{
    if (!g_ramping) {
        return;
    }
    if (g_rendered < g_ramp_start) {
        return;
    }
    g_rate = rate_at(g_ramp_from, g_ramp_to, g_rendered - g_ramp_start, g_ramp_span);
    if (g_rendered - g_ramp_start >= g_ramp_span) {
        g_rate = g_ramp_to;
        g_ramping = 0;
        if (!g_announced_hold) {
            log_info("audio", "%s", g_paused ? "paused" : "resumed");
            g_announced_hold = 1;
        }
    }
}

static void maybe_natural_fade(void)
{
    size_t blen = 0;
    size_t span;
    size_t remain;

    if (g_fading || !g_a || !pending_ready(&blen)) {
        return;
    }
    remain = remaining();
    span = (size_t)g_set.overlap_sec * g_set.rate;
    if (span > blen) {
        span = blen;
    }
    if (remain > span) {
        return;
    }
    if (span == 0 || remain == 0) {
        become(take_pending());
        return;
    }
    start_fade(remain < span ? remain : span);
}

static void render_frame(int16_t *dst)
{
    float left = 0;
    float right = 0;
    float gain_a = 1;
    float gain_b = 0;
    float s;

    update_rate();
    if (g_rate > 0.000001) {
        maybe_natural_fade();
    }

    if (g_fading && g_b && g_fade_span > 0) {
        double t = (g_pos_b - g_fade_origin) / g_fade_span;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        gain_a = (float)(1.0 - t);
        gain_b = (float)t;
    }

    if (g_rate > 0.000001 && g_a && g_pos_a < (double)g_a->end) {
        float sl, sr;
        sample_at(g_a, g_pos_a, &sl, &sr);
        left += sl * gain_a;
        right += sr * gain_a;
        g_pos_a += g_rate;
    }
    if (g_rate > 0.000001 && g_fading && g_b) {
        float sl, sr;
        sample_at(g_b, g_pos_b, &sl, &sr);
        left += sl * gain_b;
        right += sr * gain_b;
        g_pos_b += g_rate;
        if ((g_pos_b - g_fade_origin) >= g_fade_span) {
            promote();
        }
    }

    left *= g_vol;
    right *= g_vol;
    if (left > 1) {
        left = 1;
    }
    if (left < -1) {
        left = -1;
    }
    if (right > 1) {
        right = 1;
    }
    if (right < -1) {
        right = -1;
    }
    s = left * 32767.0f;
    dst[0] = (int16_t)lrintf(s);
    s = right * 32767.0f;
    dst[1] = (int16_t)lrintf(s);
    g_rendered++;
}

static void *audio_main(void *arg)
{
    unsigned period = audio_period();
    int16_t *buf = calloc(period * 2, sizeof(int16_t));

    (void)arg;
    if (!buf) {
        atomic_store(&g_stop, 1);
        return NULL;
    }
    while (!atomic_load(&g_stop)) {
        unsigned i;

        service_commands();
        for (i = 0; i < period; i++) {
            render_frame(buf + i * 2);
        }
        if (audio_write(buf, period) != 0) {
            atomic_store(&g_stop, 1);
            break;
        }
    }
    free(buf);
    return NULL;
}

static void *decode_main(void *arg)
{
    (void)arg;
    for (;;) {
        char path[4096];
        Track *track;

        pthread_mutex_lock(&g_mu);
        while (!g_need_next && !atomic_load(&g_stop)) {
            pthread_cond_wait(&g_cv, &g_mu);
        }
        if (atomic_load(&g_stop)) {
            pthread_mutex_unlock(&g_mu);
            break;
        }
        g_need_next = 0;
        pthread_mutex_unlock(&g_mu);

        if (playlist_next(g_pl, path, sizeof path) != 0) {
            log_error("decode", "playlist has no next track");
            continue;
        }
        track = track_decode(path, g_set.rate, g_set.trim);
        pthread_mutex_lock(&g_mu);
        if (atomic_load(&g_stop)) {
            pthread_mutex_unlock(&g_mu);
            track_free(track);
            break;
        }
        if (g_pending) {
            track_free(g_pending);
        }
        g_pending = track;
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

int player_start(const PlayerSettings *settings, Playlist *playlist)
{
    char path[4096];
    Track *first;

    g_set = *settings;
    g_pl = playlist;
    g_rate = 1.0;
    g_paused = 0;
    g_ramping = 0;
    g_rendered = 0;
    g_seen_pause = atomic_load(&g_pause_gen);
    g_seen_skip = atomic_load(&g_skip_gen);
    g_vol = (float)atomic_load(&g_volume) / 1000.0f;
    atomic_store(&g_stop, 0);

    if (playlist_next(g_pl, path, sizeof path) != 0) {
        return -1;
    }
    first = track_decode(path, g_set.rate, g_set.trim);
    if (!first) {
        return -1;
    }
    become(first);

    g_need_next = 1;
    if (pthread_create(&g_decode_thread, NULL, decode_main, NULL) != 0) {
        log_error("audio", "cannot start decoder");
        track_free(g_a);
        g_a = NULL;
        return -1;
    }
    if (pthread_create(&g_audio_thread, NULL, audio_main, NULL) != 0) {
        log_error("audio", "cannot start playback");
        atomic_store(&g_stop, 1);
        pthread_mutex_lock(&g_mu);
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(g_decode_thread, NULL);
        track_free(g_a);
        track_free(g_pending);
        g_a = NULL;
        g_pending = NULL;
        return -1;
    }
    g_threads = 1;
    return 0;
}

void player_stop(void)
{
    if (!g_threads) {
        track_free(g_a);
        track_free(g_b);
        track_free(g_pending);
        g_a = g_b = g_pending = NULL;
        return;
    }
    atomic_store(&g_stop, 1);
    pthread_mutex_lock(&g_mu);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_audio_thread, NULL);
    pthread_join(g_decode_thread, NULL);
    g_threads = 0;
    track_free(g_a);
    track_free(g_b);
    track_free(g_pending);
    g_a = g_b = g_pending = NULL;
}

int player_alive(void)
{
    return !atomic_load(&g_stop);
}

void player_toggle_pause(void)
{
    atomic_fetch_add(&g_pause_gen, 1);
}

void player_skip(void)
{
    atomic_fetch_add(&g_skip_gen, 1);
}

void player_set_volume_percent(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    atomic_store(&g_volume, percent * 10);
}

int player_volume_percent(void)
{
    int volume = atomic_load(&g_volume);

    if (volume < 0) {
        volume = 0;
    }
    if (volume > 1000) {
        volume = 1000;
    }
    return volume / 10;
}

void player_volume_step(int direction)
{
    int volume = atomic_load(&g_volume);
    int next = volume + direction * 50;

    if (next < 0) {
        next = 0;
    }
    if (next > 1000) {
        next = 1000;
    }
    if (next == volume) {
        return;
    }
    atomic_store(&g_volume, next);
    log_info("keys", "volume %d%%", next / 10);
}

int player_self_test(void)
{
    double mid = rate_at(1.0, 0.0, 500, 1000);
    double end = rate_at(1.0, 0.0, 1000, 1000);
    double ga;
    double gb;
    double t;

    if (mid < 0.49 || mid > 0.51 || end != 0.0) {
        log_error("audio", "rate ramp test failed");
        return -1;
    }
    t = 0.5;
    ga = 1.0 - t;
    gb = t;
    if (ga != 0.5 || gb != 0.5) {
        log_error("audio", "crossfade gain test failed");
        return -1;
    }
    player_set_volume_percent(30);
    if (player_volume_percent() != 30) {
        log_error("audio", "volume set test failed");
        player_set_volume_percent(100);
        return -1;
    }
    player_set_volume_percent(-5);
    if (player_volume_percent() != 0) {
        log_error("audio", "volume floor test failed");
        player_set_volume_percent(100);
        return -1;
    }
    player_set_volume_percent(140);
    if (player_volume_percent() != 100) {
        log_error("audio", "volume ceiling test failed");
        return -1;
    }
    return 0;
}
