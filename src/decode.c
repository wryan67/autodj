#include "decode.h"

#include "log.h"

#include <ebur128.h>
#include <mpg123.h>
#include <soxr.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static int g_mpg_ready;

int decode_init(void)
{
    int err;

    if (g_mpg_ready) {
        return 0;
    }
    err = mpg123_init();
    if (err != MPG123_OK) {
        log_error("decode", "mpg123_init failed: %s", mpg123_plain_strerror(err));
        return -1;
    }
    g_mpg_ready = 1;
    return 0;
}

void decode_shutdown(void)
{
    if (g_mpg_ready) {
        mpg123_exit();
        g_mpg_ready = 0;
    }
}

void track_free(Track *track)
{
    if (!track) {
        return;
    }
    free(track->pcm);
    free(track);
}

static void track_name(Track *track, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(track->name, sizeof track->name, "%s", base);
}

static int frame_loud(const float *pcm, size_t i)
{
    float l = pcm[i * 2];
    float r = pcm[i * 2 + 1];
    const float thresh = 0.001f; /* about -60 dBFS */

    if (l < 0) {
        l = -l;
    }
    if (r < 0) {
        r = -r;
    }
    return l > thresh || r > thresh;
}

static void apply_trim(Track *track, int enabled)
{
    size_t sustain;
    size_t pad;
    size_t i;
    size_t run = 0;
    size_t start;
    size_t end;
    int found = 0;

    track->start = 0;
    track->end = track->frames;
    if (!enabled || track->frames == 0) {
        return;
    }

    sustain = track->rate / 50; /* 20 ms */
    if (sustain < 1) {
        sustain = 1;
    }
    pad = sustain;

    for (i = 0; i < track->frames; i++) {
        if (frame_loud(track->pcm, i)) {
            run++;
            if (run >= sustain) {
                start = i + 1 - sustain;
                found = 1;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (!found) {
        track->start = 0;
        track->end = 0;
        return;
    }

    end = start;
    for (i = start; i < track->frames; i++) {
        if (frame_loud(track->pcm, i)) {
            end = i + 1;
        }
    }

    if (start > pad) {
        start -= pad;
    } else {
        start = 0;
    }
    end += pad;
    if (end > track->frames) {
        end = track->frames;
    }
    track->start = start;
    track->end = end;
}

/* EBU R128 integrated loudness of every sample, then one linear gain.
 * The gain is reduced if it would push the sample peak past full scale. */
static void apply_loudness(Track *track, int target_lufs)
{
    ebur128_state *st;
    double lufs = 0;
    double peak = 0;
    double gain;
    double applied_db;
    size_t i;
    int limited = 0;

    if (!track->pcm || track->frames == 0) {
        return;
    }
    st = ebur128_init(2, track->rate, EBUR128_MODE_I);
    if (!st) {
        log_warn("decode", "loudness meter unavailable for %s", track->name);
        return;
    }
    if (ebur128_add_frames_float(st, track->pcm, track->frames) != EBUR128_SUCCESS ||
        ebur128_loudness_global(st, &lufs) != EBUR128_SUCCESS || !isfinite(lufs)) {
        log_warn("decode", "no integrated loudness for %s", track->name);
        ebur128_destroy(&st);
        return;
    }
    ebur128_destroy(&st);

    for (i = 0; i < track->frames * 2; i++) {
        float a = track->pcm[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
    }
    gain = pow(10.0, ((double)target_lufs - lufs) / 20.0);
    if (peak > 0.0 && gain * peak > 0.99) {
        gain = 0.99 / peak;
        limited = 1;
    }
    for (i = 0; i < track->frames * 2; i++) {
        track->pcm[i] = (float)((double)track->pcm[i] * gain);
    }
    applied_db = 20.0 * log10(gain);
    log_info("decode", "loudness %s %.1f LUFS, gain %+.1f dB%s",
             track->name, lufs, applied_db, limited ? ", peak limited" : "");
}

static float *resample(const float *in, size_t in_frames, unsigned in_rate,
                       unsigned out_rate, size_t *out_frames)
{
    soxr_error_t err = NULL;
    soxr_io_spec_t io;
    soxr_quality_spec_t quality;
    soxr_t soxr;
    size_t cap;
    size_t idone = 0;
    size_t odone = 0;
    size_t odone2 = 0;
    float *out;
    soxr_error_t perr;

    if (in_rate == out_rate) {
        out = malloc(in_frames * 2 * sizeof(float));
        if (!out) {
            return NULL;
        }
        memcpy(out, in, in_frames * 2 * sizeof(float));
        *out_frames = in_frames;
        return out;
    }

    io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    quality = soxr_quality_spec(SOXR_HQ, 0);
    soxr = soxr_create(in_rate, out_rate, 2, &err, &io, &quality, NULL);
    if (!soxr) {
        log_error("decode", "resampler: %s", soxr_strerror(err));
        return NULL;
    }

    cap = (size_t)((double)in_frames * (double)out_rate / (double)in_rate) + 8192;
    out = malloc(cap * 2 * sizeof(float));
    if (!out) {
        soxr_delete(soxr);
        return NULL;
    }

    perr = soxr_process(soxr, in, in_frames, &idone, out, cap, &odone);
    if (!perr) {
        perr = soxr_process(soxr, NULL, 0, NULL, out + odone * 2, cap - odone, &odone2);
    }
    soxr_delete(soxr);
    if (perr) {
        log_error("decode", "resample failed: %s", soxr_strerror(perr));
        free(out);
        return NULL;
    }
    *out_frames = odone + odone2;
    return out;
}

Track *track_decode(const char *path, unsigned rate, int trim, int loudness, int loudness_lufs)
{
    mpg123_handle *mh;
    int err = MPG123_OK;
    long file_rate = 0;
    int channels = 0;
    int encoding = 0;
    off_t length;
    size_t cap;
    size_t got = 0;
    size_t block;
    float *native = NULL;
    float *chunk = NULL;
    Track *track;
    struct timespec t0, t1;
    unsigned ms;

    if (decode_init() != 0) {
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    mh = mpg123_new(NULL, &err);
    if (!mh) {
        log_error("decode", "mpg123_new failed: %s", mpg123_plain_strerror(err));
        return NULL;
    }
    err = mpg123_open_fixed(mh, path, MPG123_STEREO, MPG123_ENC_FLOAT_32);
    if (err != MPG123_OK) {
        log_error("decode", "cannot decode %s: %s", path, mpg123_plain_strerror(err));
        mpg123_delete(mh);
        return NULL;
    }
    err = mpg123_getformat(mh, &file_rate, &channels, &encoding);
    if (err != MPG123_OK || channels != 2 || encoding != MPG123_ENC_FLOAT_32 || file_rate <= 0) {
        log_error("decode", "unexpected format for %s", path);
        mpg123_delete(mh);
        return NULL;
    }

    length = mpg123_length(mh);
    cap = length > 0 ? (size_t)length : 65536;
    native = malloc(cap * 2 * sizeof(float));
    block = mpg123_outblock(mh);
    if (block < sizeof(float) * 2) {
        block = 1152 * 2 * sizeof(float);
    }
    chunk = malloc(block);
    if (!native || !chunk) {
        log_error("decode", "out of memory decoding %s", path);
        free(native);
        free(chunk);
        mpg123_delete(mh);
        return NULL;
    }

    for (;;) {
        size_t done = 0;
        size_t frames;

        err = mpg123_read(mh, chunk, block, &done);
        if (err != MPG123_OK && err != MPG123_DONE) {
            log_error("decode", "read failed for %s: %s", path, mpg123_plain_strerror(err));
            free(native);
            free(chunk);
            mpg123_delete(mh);
            return NULL;
        }
        frames = done / (sizeof(float) * 2);
        if (got + frames > cap) {
            size_t ncap = (got + frames) * 2 + 1024;
            float *np = realloc(native, ncap * 2 * sizeof(float));
            if (!np) {
                free(native);
                free(chunk);
                mpg123_delete(mh);
                return NULL;
            }
            native = np;
            cap = ncap;
        }
        if (frames > 0) {
            memcpy(native + got * 2, chunk, frames * 2 * sizeof(float));
            got += frames;
        }
        if (err == MPG123_DONE) {
            break;
        }
    }
    free(chunk);
    mpg123_delete(mh);

    track = calloc(1, sizeof *track);
    if (!track) {
        free(native);
        return NULL;
    }
    track->pcm = resample(native, got, (unsigned)file_rate, rate, &track->frames);
    free(native);
    if (!track->pcm) {
        free(track);
        return NULL;
    }
    track->rate = rate;
    track_name(track, path);
    if (loudness) {
        apply_loudness(track, loudness_lufs);
    }
    apply_trim(track, trim);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        long sec = t1.tv_sec - t0.tv_sec;
        long nsec = t1.tv_nsec - t0.tv_nsec;
        if (nsec < 0) {
            sec--;
            nsec += 1000000000L;
        }
        ms = (unsigned)(sec * 1000 + nsec / 1000000L);
    }
    if (track->end > track->start) {
        unsigned sec = (unsigned)((track->end - track->start) / rate);
        log_info("decode", "decoded %s in %u ms, %u:%02u audible",
                 track->name, ms, sec / 60, sec % 60);
    } else {
        log_warn("decode", "decoded %s in %u ms, but it is silent", track->name, ms);
    }
    return track;
}

int trim_self_test(void)
{
    Track track;
    size_t i;
    int rc = 0;

    memset(&track, 0, sizeof track);
    track.rate = 1000;
    track.frames = 3000;
    track.pcm = calloc(track.frames * 2, sizeof(float));
    if (!track.pcm) {
        return -1;
    }
    for (i = 1000; i < 2000; i++) {
        track.pcm[i * 2] = 0.5f;
        track.pcm[i * 2 + 1] = -0.5f;
    }
    apply_trim(&track, 1);
    /* 20 ms pad at 1000 Hz is 20 frames. */
    if (track.start != 980 || track.end != 2020) {
        log_error("decode", "trim test got %zu..%zu, expected 980..2020",
                  track.start, track.end);
        rc = -1;
    }
    apply_trim(&track, 0);
    if (track.start != 0 || track.end != track.frames) {
        log_error("decode", "trim-off test failed");
        rc = -1;
    }
    free(track.pcm);
    track.pcm = calloc(100 * 2, sizeof(float));
    track.frames = 100;
    apply_trim(&track, 1);
    if (track.end != 0) {
        log_error("decode", "silence trim test failed");
        rc = -1;
    }
    free(track.pcm);
    return rc;
}

int loudness_self_test(void)
{
    Track track;
    ebur128_state *st;
    size_t i;
    const unsigned rate = 48000;
    const size_t frames = (size_t)rate * 3;
    const double amp = pow(10.0, -23.0 / 20.0);
    double lufs = 0;
    double peak = 0;

    memset(&track, 0, sizeof track);
    track.rate = rate;
    track.frames = frames;
    snprintf(track.name, sizeof track.name, "1 kHz");
    track.pcm = malloc(frames * 2 * sizeof(float));
    if (!track.pcm) {
        log_error("decode", "loudness test out of memory");
        return -1;
    }
    for (i = 0; i < frames; i++) {
        float s = (float)(amp * sin(2.0 * M_PI * 1000.0 * (double)i / (double)rate));
        track.pcm[i * 2] = s;
        track.pcm[i * 2 + 1] = s;
    }

    st = ebur128_init(2, rate, EBUR128_MODE_I);
    if (!st || ebur128_add_frames_float(st, track.pcm, frames) != EBUR128_SUCCESS ||
        ebur128_loudness_global(st, &lufs) != EBUR128_SUCCESS) {
        log_error("decode", "loudness test could not measure the sine");
        if (st) {
            ebur128_destroy(&st);
        }
        free(track.pcm);
        return -1;
    }
    ebur128_destroy(&st);
    if (lufs < -23.2 || lufs > -22.8) {
        log_error("decode", "loudness sine test got %.2f LUFS, expected -23", lufs);
        free(track.pcm);
        return -1;
    }

    apply_loudness(&track, -18);
    st = ebur128_init(2, rate, EBUR128_MODE_I);
    lufs = 0;
    if (!st || ebur128_add_frames_float(st, track.pcm, frames) != EBUR128_SUCCESS ||
        ebur128_loudness_global(st, &lufs) != EBUR128_SUCCESS) {
        log_error("decode", "loudness test could not remeasure the sine");
        if (st) {
            ebur128_destroy(&st);
        }
        free(track.pcm);
        return -1;
    }
    ebur128_destroy(&st);
    for (i = 0; i < frames * 2; i++) {
        float a = track.pcm[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
    }
    free(track.pcm);
    if (lufs < -18.3 || lufs > -17.7 || peak >= 1.0) {
        log_error("decode", "loudness gain test got %.2f LUFS, peak %.3f", lufs, peak);
        return -1;
    }
    return 0;
}

int decode_self_test(const char *path)
{
    Track *track;

    track = track_decode(path, 44100, 1, 1, -18);
    if (!track) {
        return -1;
    }
    if (track->frames == 0 || track->end <= track->start || strcmp(track->name, "") == 0) {
        log_error("decode", "decode test produced an empty track for %s", path);
        track_free(track);
        return -1;
    }
    track_free(track);
    return 0;
}
