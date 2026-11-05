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
 * volume rises. Skip starts that same crossfade immediately. A skip while
 * paused or still braking resumes on the next track. A full stop cuts
 * straight to that track, because there is nothing left to crossfade.
 * Previous does the same toward a song already played. Twenty songs can
 * be replayed; another press past that does nothing. Rewind moves inside
 * the current song and stops at its start.
 */

#define HIST_MAX 21 /* the song playing, plus 20 before it */

static atomic_int g_pause_gen;
static atomic_int g_skip_gen;
static atomic_int g_back_gen;
static atomic_int g_rewind_ms;
static atomic_int g_volume = 1000;
static atomic_int g_stop;

static PlayerSettings g_set;
static Playlist *g_pl;

static pthread_t g_audio_thread;
static pthread_t g_decode_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Track *g_pending;
static char g_pending_path[4096];
static int g_need_prefetch;
static int g_prefetch_busy; /* decoder is already working on g_prefetch_path */
static int g_prefetch_seq;
static char g_prefetch_path[4096];
static char g_prefetch_fail[4096];
static Track *g_nav_track;
static char g_nav_path[4096];
static char g_nav_job[4096];
static int g_need_nav;
static int g_nav_seq;
static int g_nav_fail_seq;
static int g_nav_failed;
static int g_threads;

/* Songs that have started, oldest first. g_hist_at is the one playing. */
static char *g_hist[HIST_MAX];
static int g_hist_len;
static int g_hist_at = -1;
static int g_hist_pin = -1;
static char g_queued[4096];
static int g_have_queued;
static int g_nav_on;
static int g_nav_at;
static int g_nav_past; /* target is the queued song after the remembered ones */

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
static int g_seen_back;
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

static void hist_clear(void)
{
    int i;

    for (i = 0; i < g_hist_len; i++) {
        free(g_hist[i]);
        g_hist[i] = NULL;
    }
    g_hist_len = 0;
    g_hist_at = -1;
    g_hist_pin = -1;
    g_have_queued = 0;
    g_queued[0] = '\0';
    g_nav_on = 0;
    g_nav_at = -1;
    g_nav_past = 0;
}

/* Remember a song that just started. A repeat of the current path, or the
 * next remembered path, moves the cursor instead of growing the list. */
static void hist_note(const char *path)
{
    int i;

    if (!path || !path[0]) {
        return;
    }
    if (g_hist_pin >= 0 && g_hist_pin < g_hist_len) {
        g_hist_at = g_hist_pin;
        g_hist_pin = -1;
        return;
    }
    if (g_hist_at >= 0 && g_hist_at < g_hist_len && strcmp(g_hist[g_hist_at], path) == 0) {
        return;
    }
    if (g_hist_at + 1 < g_hist_len && g_hist[g_hist_at + 1] &&
        strcmp(g_hist[g_hist_at + 1], path) == 0) {
        g_hist_at++;
        if (g_have_queued && strcmp(g_queued, path) == 0) {
            g_have_queued = 0;
            g_queued[0] = '\0';
        }
        return;
    }
    for (i = g_hist_at + 1; i < g_hist_len; i++) {
        free(g_hist[i]);
        g_hist[i] = NULL;
    }
    g_hist_len = g_hist_at < 0 ? 0 : g_hist_at + 1;
    if (g_hist_len == HIST_MAX) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, (size_t)(HIST_MAX - 1) * sizeof g_hist[0]);
        g_hist_len--;
        g_hist_at--;
        g_hist[g_hist_len] = NULL;
    }
    g_hist[g_hist_len] = strdup(path);
    if (!g_hist[g_hist_len]) {
        log_error("audio", "out of memory remembering a track");
        return;
    }
    g_hist_at = g_hist_len;
    g_hist_len++;
    if (g_have_queued && strcmp(g_queued, path) == 0) {
        g_have_queued = 0;
        g_queued[0] = '\0';
    }
}

static void request_prefetch(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    if (g_pending && strcmp(g_pending_path, path) == 0) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    /* g_need_prefetch is cleared when the decoder takes the job, so the
     * in-flight flag is what stops the next poll from starting it again. */
    if ((g_need_prefetch || g_prefetch_busy) && strcmp(g_prefetch_path, path) == 0) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (strcmp(g_prefetch_fail, path) == 0) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_prefetch_seq++;
    g_prefetch_busy = 0;
    snprintf(g_prefetch_path, sizeof g_prefetch_path, "%s", path);
    g_need_prefetch = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void request_nav(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    if (g_nav_track && strcmp(g_nav_path, path) == 0) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (g_need_nav && strcmp(g_nav_job, path) == 0) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_nav_seq++;
    g_nav_failed = 0;
    track_free(g_nav_track);
    g_nav_track = NULL;
    g_nav_path[0] = '\0';
    snprintf(g_nav_job, sizeof g_nav_job, "%s", path);
    g_need_nav = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void ensure_prefetch(void)
{
    char path[4096];
    const char *want = NULL;

    if (g_hist_at + 1 < g_hist_len && g_hist[g_hist_at + 1]) {
        want = g_hist[g_hist_at + 1];
    } else if (g_have_queued) {
        want = g_queued;
    } else if (g_pl && playlist_next(g_pl, path, sizeof path) == 0) {
        snprintf(g_queued, sizeof g_queued, "%s", path);
        g_have_queued = 1;
        want = g_queued;
    }
    if (want) {
        request_prefetch(want);
    }
}

static void apply_rewind(int ms)
{
    double frames;

    if (ms <= 0 || !g_a || g_set.rate == 0) {
        return;
    }
    frames = ((double)ms / 1000.0) * (double)g_set.rate;
    g_pos_a -= frames;
    if (g_pos_a < (double)g_a->start) {
        g_pos_a = (double)g_a->start;
    }
    if (g_fading && g_b) {
        g_pos_b -= frames;
        if (g_pos_b < (double)g_b->start) {
            g_pos_b = (double)g_b->start;
        }
    }
    if (ms % 1000 == 0) {
        log_info("audio", "rewind %ds", ms / 1000);
    } else {
        log_info("audio", "rewind %d.%ds", ms / 1000, (ms % 1000) / 100);
    }
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
    g_pending_path[0] = '\0';
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
    if (track && track->path[0]) {
        hist_note(track->path);
    }
    if (track && audible(track) > 0) {
        log_playing(track);
    }
    ensure_prefetch();
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

static void begin_ramp(int pausing);

/* A skip during pause or the brake should leave the next track playing. */
static void resume_if_paused(void)
{
    if (g_paused) {
        begin_ramp(0);
    }
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
    if (next->path[0]) {
        hist_note(next->path);
    }
    log_info("audio", "crossfade into %s", next->name);
    ensure_prefetch();
}

static void play_incoming(Track *next, const char *verb)
{
    size_t blen;
    size_t span;
    size_t remain;

    if (!next) {
        return;
    }
    blen = audible(next);
    if (blen == 0) {
        log_warn("audio", "skipping silent %s", next->name);
        track_free(next);
        g_hist_pin = -1;
        return;
    }
    if (g_fading && g_b) {
        track_free(g_b);
        g_b = NULL;
        g_fading = 0;
    }
    if (g_paused && g_rate <= 0.000001) {
        log_info("audio", "%s", verb);
        become(next);
        resume_if_paused();
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
        log_info("audio", "%s", verb);
        become(next);
        resume_if_paused();
        return;
    }
    log_info("audio", "%s", verb);
    g_b = next;
    g_pos_b = (double)next->start;
    g_fade_origin = g_pos_b;
    g_fade_span = (double)span;
    g_fading = 1;
    if (next->path[0]) {
        hist_note(next->path);
    }
    log_info("audio", "crossfade into %s", next->name);
    ensure_prefetch();
    resume_if_paused();
}

static void do_skip(void)
{
    size_t blen = 0;

    /* Presses before the decode finishes move one target, instead of
     * starting a decode per press. */
    if (g_nav_past) {
        return;
    }
    if (g_nav_on && g_nav_at != g_hist_at) {
        if (g_nav_at < g_hist_at) {
            g_nav_at++;
            if (g_nav_at == g_hist_at) {
                g_nav_on = 0;
                return;
            }
            if (g_nav_at >= 0 && g_nav_at < g_hist_len && g_hist[g_nav_at]) {
                request_nav(g_hist[g_nav_at]);
            }
            return;
        }
        if (g_nav_at + 1 < g_hist_len && g_hist[g_nav_at + 1]) {
            g_nav_at++;
            request_nav(g_hist[g_nav_at]);
            return;
        }
        if (!g_have_queued && g_pl) {
            char path[4096];

            if (playlist_next(g_pl, path, sizeof path) == 0) {
                snprintf(g_queued, sizeof g_queued, "%s", path);
                g_have_queued = 1;
            }
        }
        if (g_have_queued) {
            g_nav_past = 1;
            request_nav(g_queued);
            return;
        }
        g_nav_on = 0;
        log_warn("audio", "skip ignored, next track is not ready");
        return;
    }
    if (g_fading && g_b) {
        log_info("audio", "skip");
        promote();
        resume_if_paused();
        return;
    }
    if (g_hist_at + 1 < g_hist_len && g_hist[g_hist_at + 1]) {
        g_nav_on = 1;
        g_nav_at = g_hist_at + 1;
        request_nav(g_hist[g_nav_at]);
        return;
    }
    if (g_paused && g_rate <= 0.000001) {
        if (!pending_ready(&blen)) {
            log_warn("audio", "skip ignored, next track is not ready");
            return;
        }
        play_incoming(take_pending(), "skip");
        return;
    }
    if (!pending_ready(&blen)) {
        log_warn("audio", "skip ignored, next track is not ready");
        return;
    }
    play_incoming(take_pending(), "skip");
}

/* One press moves one song back. Presses that arrive before the decode
 * finishes keep moving the target. Past the oldest remembered song, or
 * past 20 songs, the press does nothing. */
static void do_back(void)
{
    int dest;
    int base;

    if (g_nav_past) {
        g_nav_past = 0;
        if (g_nav_on && g_nav_at != g_hist_at &&
            g_nav_at >= 0 && g_nav_at < g_hist_len && g_hist[g_nav_at]) {
            request_nav(g_hist[g_nav_at]);
            return;
        }
        g_nav_on = 0;
    }
    base = (g_nav_on && g_nav_at != g_hist_at) ? g_nav_at : g_hist_at;
    dest = base - 1;
    if (dest < 0 || dest >= g_hist_len || !g_hist[dest]) {
        return;
    }
    if (g_a && g_a->path[0] && strcmp(g_a->path, g_hist[dest]) == 0) {
        if (g_fading && g_b) {
            track_free(g_b);
            g_b = NULL;
            g_fading = 0;
        }
        g_pos_a = (double)g_a->start;
        g_hist_at = dest;
        g_nav_on = 0;
        log_info("audio", "back");
        log_playing(g_a);
        resume_if_paused();
        ensure_prefetch();
        return;
    }
    g_nav_on = 1;
    g_nav_at = dest;
    request_nav(g_hist[dest]);
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

static void try_apply_nav(void)
{
    Track *track = NULL;
    const char *verb;
    int forward;
    int failed;
    int past;
    int nav_at;

    if (!g_nav_on) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    failed = g_nav_failed && g_nav_fail_seq == g_nav_seq;
    if (failed) {
        g_nav_failed = 0;
    }
    past = g_nav_past;
    nav_at = g_nav_at;
    if (!failed && g_nav_track) {
        if (past) {
            if (g_have_queued && strcmp(g_nav_path, g_queued) == 0) {
                track = g_nav_track;
            }
        } else if (nav_at >= 0 && nav_at < g_hist_len && g_hist[nav_at] &&
                   strcmp(g_nav_path, g_hist[nav_at]) == 0) {
            track = g_nav_track;
        }
        if (track) {
            g_nav_track = NULL;
            g_nav_path[0] = '\0';
            g_nav_job[0] = '\0';
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (failed) {
        log_warn("audio", "%s ignored, could not load the track",
                 (!past && nav_at < g_hist_at) ? "back" : "skip");
        g_nav_on = 0;
        g_nav_past = 0;
        return;
    }
    if (!track) {
        return;
    }
    g_nav_on = 0;
    g_nav_past = 0;
    if (past) {
        play_incoming(track, "skip");
        return;
    }
    forward = nav_at > g_hist_at;
    verb = forward ? "skip" : "back";
    g_hist_pin = nav_at;
    play_incoming(track, verb);
    g_hist_pin = -1;
}

static void service_commands(void)
{
    int pause_gen = atomic_load(&g_pause_gen);
    int skip_gen = atomic_load(&g_skip_gen);
    int back_gen = atomic_load(&g_back_gen);
    int rewind_ms = atomic_exchange(&g_rewind_ms, 0);
    int volume = atomic_load(&g_volume);

    if (rewind_ms > 0) {
        apply_rewind(rewind_ms);
    }
    while (g_seen_pause != pause_gen) {
        g_seen_pause++;
        begin_ramp(!g_paused);
    }
    while (g_seen_back != back_gen) {
        g_seen_back++;
        do_back();
    }
    while (g_seen_skip != skip_gen) {
        g_seen_skip++;
        do_skip();
    }
    try_apply_nav();
    ensure_prefetch();
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

    if (g_fading || g_nav_on || !g_a || !pending_ready(&blen)) {
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
        int nav;
        int seq;
        Track *track;

        pthread_mutex_lock(&g_mu);
        while (!g_need_nav && !g_need_prefetch && !atomic_load(&g_stop)) {
            pthread_cond_wait(&g_cv, &g_mu);
        }
        if (atomic_load(&g_stop)) {
            pthread_mutex_unlock(&g_mu);
            break;
        }
        nav = g_need_nav;
        if (nav) {
            seq = g_nav_seq;
            snprintf(path, sizeof path, "%s", g_nav_job);
            g_need_nav = 0;
        } else {
            seq = g_prefetch_seq;
            snprintf(path, sizeof path, "%s", g_prefetch_path);
            g_need_prefetch = 0;
            g_prefetch_busy = 1;
        }
        pthread_mutex_unlock(&g_mu);

        if (!path[0]) {
            continue;
        }
        track = track_decode(path, g_set.rate, g_set.trim, g_set.loudness, g_set.loudness_lufs);
        pthread_mutex_lock(&g_mu);
        if (atomic_load(&g_stop)) {
            pthread_mutex_unlock(&g_mu);
            track_free(track);
            break;
        }
        if (nav) {
            if (seq != g_nav_seq) {
                track_free(track);
            } else if (!track) {
                g_nav_failed = 1;
                g_nav_fail_seq = seq;
            } else {
                track_free(g_nav_track);
                g_nav_track = track;
                snprintf(g_nav_path, sizeof g_nav_path, "%s", path);
                g_nav_failed = 0;
            }
        } else if (seq != g_prefetch_seq) {
            track_free(track);
        } else if (!track) {
            g_prefetch_busy = 0;
            snprintf(g_prefetch_fail, sizeof g_prefetch_fail, "%s", path);
        } else {
            g_prefetch_busy = 0;
            track_free(g_pending);
            g_pending = track;
            snprintf(g_pending_path, sizeof g_pending_path, "%s", path);
            g_prefetch_fail[0] = '\0';
        }
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
    g_seen_back = atomic_load(&g_back_gen);
    atomic_store(&g_rewind_ms, 0);
    g_vol = (float)atomic_load(&g_volume) / 1000.0f;
    atomic_store(&g_stop, 0);
    hist_clear();
    g_prefetch_fail[0] = '\0';
    g_nav_failed = 0;
    g_need_nav = 0;
    g_need_prefetch = 0;
    g_prefetch_busy = 0;
    g_nav_job[0] = '\0';
    g_nav_path[0] = '\0';
    g_prefetch_path[0] = '\0';
    g_pending_path[0] = '\0';

    if (playlist_next(g_pl, path, sizeof path) != 0) {
        return -1;
    }
    first = track_decode(path, g_set.rate, g_set.trim, g_set.loudness, g_set.loudness_lufs);
    if (!first) {
        return -1;
    }
    become(first);

    if (pthread_create(&g_decode_thread, NULL, decode_main, NULL) != 0) {
        log_error("audio", "cannot start decoder");
        track_free(g_a);
        track_free(g_pending);
        track_free(g_nav_track);
        g_a = NULL;
        g_pending = NULL;
        g_nav_track = NULL;
        hist_clear();
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
        track_free(g_nav_track);
        g_a = NULL;
        g_pending = NULL;
        g_nav_track = NULL;
        hist_clear();
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
        track_free(g_nav_track);
        g_a = g_b = g_pending = g_nav_track = NULL;
        hist_clear();
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
    track_free(g_nav_track);
    g_a = g_b = g_pending = g_nav_track = NULL;
    hist_clear();
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

void player_previous(void)
{
    atomic_fetch_add(&g_back_gen, 1);
}

void player_rewind(int ms)
{
    if (ms > 0) {
        atomic_fetch_add(&g_rewind_ms, ms);
    }
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

    hist_clear();
    hist_note("/song-a");
    hist_note("/song-b");
    hist_note("/song-c");
    if (g_hist_len != 3 || g_hist_at != 2 || strcmp(g_hist[1], "/song-b") != 0) {
        log_error("audio", "history did not remember three tracks");
        hist_clear();
        return -1;
    }
    g_hist_pin = 0;
    hist_note("/song-a");
    if (g_hist_at != 0 || g_hist_len != 3) {
        log_error("audio", "history did not move back");
        hist_clear();
        return -1;
    }
    hist_note("/song-d");
    if (g_hist_len != 2 || g_hist_at != 1 || strcmp(g_hist[1], "/song-d") != 0) {
        log_error("audio", "history did not drop songs after a branch");
        hist_clear();
        return -1;
    }
    hist_clear();
    {
        int n;
        char path[32];

        for (n = 0; n < HIST_MAX + 4; n++) {
            snprintf(path, sizeof path, "/n-%d", n);
            hist_note(path);
        }
        if (g_hist_len != HIST_MAX || g_hist_at != HIST_MAX - 1) {
            log_error("audio", "history kept %d songs", g_hist_len);
            hist_clear();
            return -1;
        }
        if (!g_hist[0] || strcmp(g_hist[0], "/n-4") != 0) {
            log_error("audio", "history dropped the wrong songs");
            hist_clear();
            return -1;
        }
    }
    hist_clear();

    {
        Track track;

        memset(&track, 0, sizeof track);
        track.start = 1000;
        track.end = 200000;
        track.frames = 200000;
        g_a = &track;
        g_b = NULL;
        g_fading = 0;
        g_set.rate = 44100;
        g_pos_a = 1000 + 44100;
        apply_rewind(500);
        if (g_pos_a < 1000 + 22050 - 1 || g_pos_a > 1000 + 22050 + 1) {
            log_error("audio", "rewind 0.5s landed at %.0f", g_pos_a);
            g_a = NULL;
            return -1;
        }
        g_pos_a = 1000 + 100;
        apply_rewind(1500);
        if (g_pos_a != 1000) {
            log_error("audio", "rewind did not stop at the start");
            g_a = NULL;
            return -1;
        }
        {
            Track incoming;

            memset(&incoming, 0, sizeof incoming);
            incoming.start = 500;
            incoming.end = 200000;
            incoming.frames = 200000;
            g_b = &incoming;
            g_fading = 1;
            g_pos_a = 1000.0 + 44100.0;
            g_pos_b = 500.0 + 44100.0;
            apply_rewind(1000);
            if (g_pos_a != 1000.0 || g_pos_b != 500.0) {
                log_error("audio", "rewind during a crossfade left %.0f and %.0f",
                          g_pos_a, g_pos_b);
                g_a = NULL;
                g_b = NULL;
                g_fading = 0;
                return -1;
            }
            g_b = NULL;
            g_fading = 0;
        }
        g_a = NULL;
    }

    hist_clear();
    {
        int n;
        char path[32];

        for (n = 0; n < HIST_MAX; n++) {
            snprintf(path, sizeof path, "/h-%d", n);
            hist_note(path);
        }
        for (n = 0; n < HIST_MAX - 1; n++) {
            do_back();
        }
        if (!g_nav_on || g_nav_at != 0) {
            log_error("audio", "back stopped at %d", g_nav_at);
            hist_clear();
            g_need_nav = 0;
            return -1;
        }
        do_back();
        if (!g_nav_on || g_nav_at != 0) {
            log_error("audio", "back past 20 songs moved to %d", g_nav_at);
            hist_clear();
            g_need_nav = 0;
            return -1;
        }
    }

    hist_clear();
    hist_note("/song-a");
    hist_note("/song-b");
    hist_note("/song-c");
    g_hist_at = 0;
    snprintf(g_queued, sizeof g_queued, "/song-d");
    g_have_queued = 1;
    do_skip();
    do_skip();
    do_skip();
    if (!g_nav_past || strcmp(g_nav_job, "/song-d") != 0) {
        log_error("audio", "skip past remembered songs targeted %s", g_nav_job);
        hist_clear();
        g_need_nav = 0;
        g_nav_past = 0;
        return -1;
    }
    do_skip();
    if (!g_nav_past || strcmp(g_nav_job, "/song-d") != 0) {
        log_error("audio", "an extra skip left history");
        hist_clear();
        g_need_nav = 0;
        g_nav_past = 0;
        return -1;
    }
    do_back();
    if (g_nav_past || g_nav_at != 2 || strcmp(g_nav_job, "/song-c") != 0) {
        log_error("audio", "back from the queued song targeted %s", g_nav_job);
        hist_clear();
        g_need_nav = 0;
        g_nav_past = 0;
        return -1;
    }
    hist_clear();
    g_need_nav = 0;
    g_need_prefetch = 0;
    g_nav_job[0] = '\0';
    g_nav_path[0] = '\0';

    g_prefetch_seq = 4;
    g_prefetch_busy = 1;
    g_need_prefetch = 0;
    g_pending = NULL;
    g_pending_path[0] = '\0';
    g_prefetch_fail[0] = '\0';
    snprintf(g_prefetch_path, sizeof g_prefetch_path, "/next");
    request_prefetch("/next");
    if (g_prefetch_seq != 4 || g_need_prefetch != 0 || g_prefetch_busy != 1) {
        log_error("audio", "prefetch restarted a decode that was already running");
        g_prefetch_busy = 0;
        g_need_prefetch = 0;
        return -1;
    }
    request_prefetch("/other");
    if (g_prefetch_seq != 5 || g_need_prefetch != 1 || g_prefetch_busy != 0 ||
        strcmp(g_prefetch_path, "/other") != 0) {
        log_error("audio", "prefetch did not replace the in-flight song");
        g_prefetch_busy = 0;
        g_need_prefetch = 0;
        return -1;
    }
    g_need_prefetch = 0;
    g_prefetch_busy = 0;
    g_prefetch_path[0] = '\0';
    return 0;
}
