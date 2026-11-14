#include "fx.h"

#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reverb is a plate reverb: input smoothing, four diffusing allpasses, and
 * two cross-fed tanks with slowly moving allpasses, read at several points
 * for a wide stereo tail. Pitch change is a phase vocoder, so a full mix
 * keeps its steady tones and only the pitch moves. Autopan moves the
 * balance from one side to the other and back, in time with the beat.
 * The phaser is a chain of allpass filters whose notches sweep up and down
 * once per measure.
 * Chorus mixes in a few copies that are a little late and slowly drifting
 * in pitch, so one part sounds like several. Flanger does the same kind of
 * mix with a much shorter delay, so the copy cancels the original in a
 * comb that sweeps and whooshes.
 * Underwater is a steep low-pass with a resonant bump, which leaves only a
 * muffled, boomy body, plus a slow pitch wobble from a moving delay.
 * Lasers adds falling zaps on top of the song. Each one is a low tone that
 * falls the whole time it is loud. They often come as a pair, then a gap.
 * Turning lasers off lets a zap that is already sounding finish, and a
 * second one that is about to follow it. The song itself is left as it is.
 */

#define AP_DELAY 512
#define PH_STAGES 12
#define PV_OSAMP 8
#define PV_PI 3.14159265358979323846f
#define LZ_VOICES 16

/* One zap. Dead when amp has fallen away.
 * freq eases toward target across the whole note. */
typedef struct Laser {
    float phase;
    float freq;
    float target;
    float settle;
    float amp;
    float decay;
    float attack;
    float attack_step;
    float gain_l;
    float gain_r;
} Laser;

typedef struct Ring {
    float *buf;
    int len;
    int pos;
} Ring;

typedef struct PvCh {
    float *in_fifo;
    float *out_fifo;
    float *out_accum;
    float *last_phase;
    float *sum_phase;
    int rover;
} PvCh;

struct FxState {
    unsigned rate;
    float *rv_block;
    float *pv_block;
    FxParams p;

    Ring rv_pre[4];
    Ring rv_mod[2];
    Ring rv_d1[2];
    Ring rv_ap[2];
    Ring rv_d2[2];
    int rv_dpre[4];
    int rv_dmod[2];
    int rv_dd1[2];
    int rv_dap[2];
    int rv_dd2[2];
    int rv_tap[2][7];
    float rv_excursion;
    float rv_lp_in;
    float rv_lp[2];
    float rv_lfo[2];
    float rv_send;
    int rv_on;
    int rv_live;

    int pv_n;
    int pv_step;
    int pv_latency;
    PvCh pv[2];
    float *pv_win;
    float *pv_cos;
    float *pv_sin;
    int *pv_rev;
    float *pv_work;
    float *pv_mag;
    float *pv_frq;
    float *pv_smag;
    float *pv_sfrq;
    float *pv_env;
    float *pv_env2;
    int ps_warm;
    float ps_mix;
    int ps_on;
    int ps_wet;

    float ap_buf[2][AP_DELAY];
    int ap_w;
    double ap_phase;
    float ap_mix;
    float ph_x[2][PH_STAGES];
    float ph_y[2][PH_STAGES];
    float ph_fb[2];
    double ph_phase;
    float ph_mix;
    float ap_ret_amp;
    float ap_ret_th;
    float ap_ret_w;
    int ap_ret;
    unsigned ap_rng; /* 0 always starts to the left (self test). */
    int ph_on;
    int ph_live;
    float beat_sec;
    int ap_on;
    int ap_live;

    float *ch_buf[2];
    int ch_len;
    int ch_w;
    double ch_phase;
    float ch_mix;
    int ch_on;
    int ch_live;
    float *fl_buf[2];
    int fl_len;
    int fl_w;
    double fl_phase;
    float fl_mix;
    int fl_on;
    int fl_live;
    float *uw_buf[2];
    int uw_len;
    int uw_w;
    double uw_phase;
    float uw_mix;
    int uw_on;
    int uw_live;
    float uw_z[2][2][2];  /* [channel][stage] state of the two low-passes */
    float uw_coef[2][5];  /* [stage] b0 b1 b2 a1 a2 */
    float uw_fc;          /* cutoff and resonance the coefficients are for */
    float uw_q;
    Laser lz[LZ_VOICES];
    float lz_wait; /* samples until the next zap */
    int lz_pair;   /* the next gap is the long one before a pair */
    int lz_follow; /* a second zap is already due, a short gap from now */
    float lz_mix;
    int lz_on;
    int lz_live;
    unsigned lz_rng;
};

/* Plate reverb lengths in samples at 29761 Hz. */
#define RV_BASE_RATE 29761.0
static const int g_pre_len[4] = {142, 107, 379, 277};
static const float g_pre_g[4] = {0.75f, 0.75f, 0.625f, 0.625f};
static const int g_mod_len[2] = {672, 908};
static const int g_d1_len[2] = {4453, 4217};
static const int g_ap_len[2] = {1800, 2656};
static const int g_d2_len[2] = {3720, 3163};

/* Tap points: +d1, +d1, -ap, +d2, -d1, -ap, -d2 of the opposite or own tank. */
static const int g_tap_len[2][7] = {
    {266, 2974, 1913, 1996, 1990, 187, 1066},
    {353, 3627, 1228, 2673, 2111, 335, 121}
};

void fx_defaults(FxParams *p)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof *p);
    p->rv_decay = 0.5f;
    p->rv_bw = 1.0f;
    p->rv_damp = 0.0f;
    p->rv_send = 0.5f;
    p->rv_send_min = 0.0f;
    p->rv_send_max = 1.0f;
    p->ps_pitch = 1.0f;
    p->ps_range = 1.0f;
    p->ps_semi = 1;
    p->ps_formant = 0;
    p->ap_period = 4.0f;
    p->ap_smooth = 0.5f;
    p->ap_distance = 75.0f;
    p->ph_period = 1.0f;
    p->ph_low = 300.0f;
    p->ph_high = 3000.0f;
    p->ph_stages = 6;
    p->ph_feedback = 0.5f;
    p->ph_blend = 50.0f;
    p->ph_boost = 15.0f;
    p->ch_period = 1.0f;
    p->ch_delay = 20.0f;
    p->ch_depth = 5.0f;
    p->ch_voices = 2;
    p->ch_mix = 50.0f;
    p->fl_period = 2.0f;
    p->fl_delay = 1.0f;
    p->fl_depth = 0.5f;
    p->fl_feedback = 0.5f;
    p->fl_mix = 50.0f;
    p->uw_cutoff = 500.0f;
    p->uw_res = 0.4f;
    p->uw_period = 0.5f;
    p->uw_wobble = 1.5f;
    p->uw_mix = 100.0f;
    p->uw_boost = 20.0f;
    p->lz_rate = 4.0f;
    p->lz_low = 280.0f;
    p->lz_high = 700.0f;
    p->lz_length = 260.0f;
    p->lz_level = 30.0f;
}

static int scaled(unsigned rate, int n)
{
    int v = (int)((double)n * (double)rate / RV_BASE_RATE + 0.5);

    return v < 2 ? 2 : v;
}

static void ring_init(Ring *r, float **cursor, int len, int *total)
{
    r->len = len;
    r->pos = 0;
    if (cursor) {
        r->buf = *cursor;
        *cursor += len;
    } else {
        *total += len;
    }
}

static float ring_at(const Ring *r, int d)
{
    int i = r->pos - d;

    if (i < 0) {
        i += r->len;
    }
    return r->buf[i];
}

static float ring_at_frac(const Ring *r, float d)
{
    int whole = (int)d;
    float frac = d - (float)whole;
    float a = ring_at(r, whole);
    float b = ring_at(r, whole + 1);

    return a + (b - a) * frac;
}

static void ring_push(Ring *r, float x)
{
    r->buf[r->pos] = x;
    r->pos++;
    if (r->pos >= r->len) {
        r->pos = 0;
    }
}

static void ring_clear(Ring *r)
{
    memset(r->buf, 0, (size_t)r->len * sizeof(float));
    r->pos = 0;
}

/* Lays out every reverb delay line. With cursor NULL it only adds up the
 * floats needed.
 */
static void rv_layout(FxState *st, float **cursor, int *total)
{
    unsigned rate = st->rate;
    int i;
    int s;
    int k;

    for (i = 0; i < 4; i++) {
        st->rv_dpre[i] = scaled(rate, g_pre_len[i]);
        ring_init(&st->rv_pre[i], cursor, st->rv_dpre[i] + 4, total);
    }
    st->rv_excursion = (float)(8.0 * (double)rate / RV_BASE_RATE);
    for (s = 0; s < 2; s++) {
        st->rv_dmod[s] = scaled(rate, g_mod_len[s]);
        st->rv_dd1[s] = scaled(rate, g_d1_len[s]);
        st->rv_dap[s] = scaled(rate, g_ap_len[s]);
        st->rv_dd2[s] = scaled(rate, g_d2_len[s]);
        ring_init(&st->rv_mod[s], cursor,
                  st->rv_dmod[s] + (int)st->rv_excursion + 6, total);
        ring_init(&st->rv_d1[s], cursor, st->rv_dd1[s] + 4, total);
        ring_init(&st->rv_ap[s], cursor, st->rv_dap[s] + 4, total);
        ring_init(&st->rv_d2[s], cursor, st->rv_dd2[s] + 4, total);
        for (k = 0; k < 7; k++) {
            st->rv_tap[s][k] = scaled(rate, g_tap_len[s][k]);
        }
    }
}

static float *take_floats(float **cursor, int n)
{
    float *p = *cursor;

    *cursor += n;
    return p;
}

static int is_pow2(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

FxState *fx_new(unsigned rate)
{
    FxState *st;
    float *cursor;
    int total = 0;
    int pv_total;
    int n;
    int i;
    int ch;

    if (rate < 8000) {
        rate = 8000;
    }
    if (rate > 192000) {
        rate = 192000;
    }
    st = calloc(1, sizeof *st);
    if (!st) {
        return NULL;
    }
    st->rate = rate;
    st->ap_rng = (unsigned)time(NULL) ^ (unsigned)(clock() << 8) ^ 0x9e3779b9u;
    if (!st->ap_rng) {
        st->ap_rng = 1;
    }
    st->lz_rng = (unsigned)time(NULL) ^ (unsigned)(clock() << 16) ^ 0xA5A5A5A5u;
    if (!st->lz_rng) {
        st->lz_rng = 0x6d2b79f5u;
    }

    rv_layout(st, NULL, &total);
    st->rv_block = calloc((size_t)total, sizeof(float));
    if (!st->rv_block) {
        free(st);
        return NULL;
    }
    cursor = st->rv_block;
    rv_layout(st, &cursor, &total);

    n = rate > 70000 ? 4096 : 2048;
    if (!is_pow2(n)) {
        n = 2048;
    }
    st->pv_n = n;
    st->pv_step = n / PV_OSAMP;
    st->pv_latency = n - st->pv_step;
    /* Per channel: in n, out step, accum 2n, two phase arrays n/2+1. Shared:
     * window, cos, sin, work 2n, six spectrum arrays.
     */
    pv_total = 2 * (n + st->pv_step + 2 * n + 2 * (n / 2 + 1)) +
               3 * n + 2 * n + 6 * (n / 2 + 1);
    st->pv_block = calloc((size_t)pv_total, sizeof(float));
    st->pv_rev = calloc((size_t)n, sizeof(int));
    if (!st->pv_block || !st->pv_rev) {
        free(st->pv_block);
        free(st->pv_rev);
        free(st->rv_block);
        free(st);
        return NULL;
    }
    cursor = st->pv_block;
    for (ch = 0; ch < 2; ch++) {
        PvCh *c = &st->pv[ch];

        c->in_fifo = take_floats(&cursor, n);
        c->out_fifo = take_floats(&cursor, st->pv_step);
        c->out_accum = take_floats(&cursor, 2 * n);
        c->last_phase = take_floats(&cursor, n / 2 + 1);
        c->sum_phase = take_floats(&cursor, n / 2 + 1);
    }
    st->pv_win = take_floats(&cursor, n);
    st->pv_cos = take_floats(&cursor, n);
    st->pv_sin = take_floats(&cursor, n);
    st->pv_work = take_floats(&cursor, 2 * n);
    st->pv_mag = take_floats(&cursor, n / 2 + 1);
    st->pv_frq = take_floats(&cursor, n / 2 + 1);
    st->pv_smag = take_floats(&cursor, n / 2 + 1);
    st->pv_sfrq = take_floats(&cursor, n / 2 + 1);
    st->pv_env = take_floats(&cursor, n / 2 + 1);
    st->pv_env2 = take_floats(&cursor, n / 2 + 1);
    for (i = 0; i < n; i++) {
        int bits = 0;
        int r = 0;
        int v = i;

        while ((1 << bits) < n) {
            r = (r << 1) | (v & 1);
            v >>= 1;
            bits++;
        }
        st->pv_rev[i] = r;
        st->pv_win[i] = 0.5f - 0.5f * cosf(2.0f * PV_PI * (float)i / (float)n);
        st->pv_cos[i] = cosf(2.0f * PV_PI * (float)i / (float)n);
        st->pv_sin[i] = sinf(2.0f * PV_PI * (float)i / (float)n);
    }
    /* Chorus needs room for a 40 ms delay plus its swing. Flanger stays
     * under 10 ms, which is what keeps its notches up in the audible band.
     */
    st->ch_len = (int)((double)rate * 0.070) + 8;
    st->fl_len = (int)((double)rate * 0.020) + 8;
    st->uw_len = (int)((double)rate * 0.020) + 8;
    st->ch_buf[0] = calloc((size_t)st->ch_len * 2, sizeof(float));
    st->fl_buf[0] = calloc((size_t)st->fl_len * 2, sizeof(float));
    st->uw_buf[0] = calloc((size_t)st->uw_len * 2, sizeof(float));
    if (!st->ch_buf[0] || !st->fl_buf[0] || !st->uw_buf[0]) {
        free(st->ch_buf[0]);
        free(st->fl_buf[0]);
        free(st->uw_buf[0]);
        free(st->pv_block);
        free(st->pv_rev);
        free(st->rv_block);
        free(st);
        return NULL;
    }
    st->ch_buf[1] = st->ch_buf[0] + st->ch_len;
    st->fl_buf[1] = st->fl_buf[0] + st->fl_len;
    st->uw_buf[1] = st->uw_buf[0] + st->uw_len;
    fx_defaults(&st->p);
    return st;
}

void fx_free(FxState *st)
{
    if (!st) {
        return;
    }
    free(st->rv_block);
    free(st->pv_block);
    free(st->pv_rev);
    free(st->ch_buf[0]);
    free(st->fl_buf[0]);
    free(st->uw_buf[0]);
    free(st);
}

static void rv_clear(FxState *st)
{
    int s;
    int i;

    for (i = 0; i < 4; i++) {
        ring_clear(&st->rv_pre[i]);
    }
    for (s = 0; s < 2; s++) {
        ring_clear(&st->rv_mod[s]);
        ring_clear(&st->rv_d1[s]);
        ring_clear(&st->rv_ap[s]);
        ring_clear(&st->rv_d2[s]);
        st->rv_lp[s] = 0;
    }
    st->rv_lp_in = 0;
}

static void ps_clear(FxState *st)
{
    int ch;
    int n = st->pv_n;

    for (ch = 0; ch < 2; ch++) {
        PvCh *c = &st->pv[ch];

        memset(c->in_fifo, 0, (size_t)n * sizeof(float));
        memset(c->out_fifo, 0, (size_t)st->pv_step * sizeof(float));
        memset(c->out_accum, 0, (size_t)2 * n * sizeof(float));
        memset(c->last_phase, 0, (size_t)(n / 2 + 1) * sizeof(float));
        memset(c->sum_phase, 0, (size_t)(n / 2 + 1) * sizeof(float));
        c->rover = st->pv_latency;
    }
    st->ps_warm = 0;
    st->ps_wet = 0;
}

static void ap_clear(FxState *st)
{
    memset(st->ap_buf, 0, sizeof st->ap_buf);
    st->ap_w = 0;
    st->ap_phase = 0;
    st->ap_ret = 0;
    st->ap_ret_amp = 0;
    st->ap_ret_th = 0;
    st->ap_ret_w = 0;
}

static void ph_clear(FxState *st)
{
    memset(st->ph_x, 0, sizeof st->ph_x);
    memset(st->ph_y, 0, sizeof st->ph_y);
    memset(st->ph_fb, 0, sizeof st->ph_fb);
    st->ph_phase = 0;
}

static void ch_clear(FxState *st)
{
    if (st->ch_buf[0] && st->ch_len > 0) {
        memset(st->ch_buf[0], 0, (size_t)st->ch_len * 2 * sizeof(float));
    }
    st->ch_w = 0;
    st->ch_phase = 0;
}

static void uw_clear(FxState *st)
{
    if (st->uw_buf[0] && st->uw_len > 0) {
        memset(st->uw_buf[0], 0, (size_t)st->uw_len * 2 * sizeof(float));
    }
    memset(st->uw_z, 0, sizeof st->uw_z);
    st->uw_w = 0;
    st->uw_phase = 0;
    st->uw_fc = -1;
}

static void fl_clear(FxState *st)
{
    if (st->fl_buf[0] && st->fl_len > 0) {
        memset(st->fl_buf[0], 0, (size_t)st->fl_len * 2 * sizeof(float));
    }
    st->fl_w = 0;
    st->fl_phase = 0;
}

static void lz_clear(FxState *st)
{
    memset(st->lz, 0, sizeof st->lz);
    st->lz_wait = 0;
    st->lz_pair = 0;
    st->lz_follow = 0;
}

/* A zap is still sounding, or the second of a pair is already coming. */
static int lz_sounding(const FxState *st)
{
    int i;

    if (st->lz_follow) {
        return 1;
    }
    for (i = 0; i < LZ_VOICES; i++) {
        if (st->lz[i].amp >= 1.0e-5f) {
            return 1;
        }
    }
    return 0;
}

void fx_setup(FxState *st, const FxParams *p)
{
    if (!st) {
        return;
    }
    if (p) {
        st->p = *p;
    } else {
        fx_defaults(&st->p);
    }
    rv_clear(st);
    ps_clear(st);
    ap_clear(st);
    ph_clear(st);
    ch_clear(st);
    fl_clear(st);
    uw_clear(st);
    lz_clear(st);
    st->rv_send = 0;
    st->rv_live = 0;
    st->rv_lfo[0] = 0;
    st->rv_lfo[1] = 1.7f;
    st->ps_mix = 0;
    st->ap_mix = 0;
    st->ap_live = 0;
    st->ph_mix = 0;
    st->ph_live = 0;
    st->ch_mix = 0;
    st->ch_live = 0;
    st->fl_mix = 0;
    st->fl_live = 0;
    st->uw_mix = 0;
    st->uw_live = 0;
    st->lz_mix = 0;
    st->lz_live = 0;
    st->rv_on = st->p.reverb_on ? 1 : 0;
    st->ps_on = st->p.pitch_on ? 1 : 0;
    st->ap_on = st->p.autopan_on ? 1 : 0;
    st->ph_on = st->p.phaser_on ? 1 : 0;
    st->ch_on = st->p.chorus_on ? 1 : 0;
    st->fl_on = st->p.flanger_on ? 1 : 0;
    st->uw_on = st->p.underwater_on ? 1 : 0;
    st->lz_on = st->p.lasers_on ? 1 : 0;
}

void fx_set_send(FxState *st, float send)
{
    if (st) {
        st->p.rv_send = send;
    }
}

void fx_set_lasers(FxState *st, float level)
{
    if (st) {
        st->p.lz_level = level;
    }
}

void fx_set_on(FxState *st, int reverb, int pitch, int autopan, int phaser,
               int chorus, int flanger, int underwater, int lasers)
{
    if (!st) {
        return;
    }
    st->rv_on = reverb ? 1 : 0;
    st->ps_on = pitch ? 1 : 0;
    st->ap_on = autopan ? 1 : 0;
    st->ph_on = phaser ? 1 : 0;
    st->ch_on = chorus ? 1 : 0;
    st->fl_on = flanger ? 1 : 0;
    st->uw_on = underwater ? 1 : 0;
    st->lz_on = lasers ? 1 : 0;
}

void fx_set_beat(FxState *st, float beat_seconds)
{
    if (!st) {
        return;
    }
    st->beat_sec = beat_seconds > 0 && beat_seconds < 10.0f ? beat_seconds : 0;
}

static float clamp_unit(float x)
{
    if (x < 0) {
        return 0;
    }
    if (x > 1) {
        return 1;
    }
    return x;
}

static float fin(float x)
{
    if (x != x || x > 8.0f || x < -8.0f) {
        return 0;
    }
    return x;
}

static float ring_allpass(Ring *r, int d, float x, float g)
{
    float y = -g * x + ring_at(r, d);

    ring_push(r, x + g * y);
    return fin(y);
}

static void process_reverb(FxState *st, float *left, float *right)
{
    float in[2];
    float send;
    float bwc;
    float decay;
    float damp;
    float diff2;
    float x;
    float tank_in[2];
    float end[2];
    float wet[2];
    float outv[2];
    int i;
    int s;

    in[0] = *left;
    in[1] = *right;
    if (st->rv_on && !st->rv_live) {
        rv_clear(st);
        st->rv_live = 1;
    }
    send = st->rv_on ? clamp_unit(st->p.rv_send) : 0;
    st->rv_send += 0.0025f * (send - st->rv_send);
    if (!st->rv_on && st->rv_send < 0.00005f) {
        st->rv_send = 0;
        if (st->rv_live) {
            rv_clear(st);
            st->rv_live = 0;
        }
        return;
    }
    if (st->rv_send == 0 && !st->rv_on) {
        return;
    }

    /* Decay maps to the loop gain of each tank: 0.3 to 0.95. */
    decay = 0.3f + 0.65f * clamp_unit(st->p.rv_decay);
    damp = 0.9f * clamp_unit(st->p.rv_damp);
    bwc = 0.1f + 0.8995f * clamp_unit(st->p.rv_bw);
    diff2 = decay + 0.15f;
    if (diff2 < 0.25f) {
        diff2 = 0.25f;
    }
    if (diff2 > 0.5f) {
        diff2 = 0.5f;
    }

    x = 0.5f * (in[0] + in[1]);
    st->rv_lp_in += bwc * (x - st->rv_lp_in);
    x = 0.5f * st->rv_lp_in;
    for (i = 0; i < 4; i++) {
        x = ring_allpass(&st->rv_pre[i], st->rv_dpre[i], x, g_pre_g[i]);
    }

    for (s = 0; s < 2; s++) {
        end[s] = ring_at(&st->rv_d2[s], st->rv_dd2[s]);
    }
    tank_in[0] = x + decay * end[1];
    tank_in[1] = x + decay * end[0];

    for (s = 0; s < 2; s++) {
        float lfo;
        float delayed;
        float y;
        float v;
        float z;

        st->rv_lfo[s] += (float)(2.0 * PV_PI * (s ? 0.83 : 1.0) / (double)st->rate);
        if (st->rv_lfo[s] > 2.0f * PV_PI) {
            st->rv_lfo[s] -= 2.0f * PV_PI;
        }
        lfo = (float)st->rv_dmod[s] + st->rv_excursion * (1.0f + sinf(st->rv_lfo[s])) * 0.5f;
        delayed = ring_at_frac(&st->rv_mod[s], lfo);
        y = fin(-0.7f * tank_in[s] + delayed);
        ring_push(&st->rv_mod[s], tank_in[s] + 0.7f * y);

        v = ring_at(&st->rv_d1[s], st->rv_dd1[s]);
        ring_push(&st->rv_d1[s], y);
        st->rv_lp[s] += (1.0f - damp) * (v - st->rv_lp[s]);
        if (st->rv_lp[s] > -1e-20f && st->rv_lp[s] < 1e-20f) {
            st->rv_lp[s] = 0;
        }
        z = st->rv_lp[s] * decay;
        z = ring_allpass(&st->rv_ap[s], st->rv_dap[s], z, diff2);
        ring_push(&st->rv_d2[s], z);
    }

    /* Left reads mostly the right tank and the other way round. */
    wet[0] = ring_at(&st->rv_d1[1], st->rv_tap[1][0]) +
             ring_at(&st->rv_d1[1], st->rv_tap[1][1]) -
             ring_at(&st->rv_ap[1], st->rv_tap[1][2]) +
             ring_at(&st->rv_d2[1], st->rv_tap[1][3]) -
             ring_at(&st->rv_d1[0], st->rv_tap[0][4]) -
             ring_at(&st->rv_ap[0], st->rv_tap[0][5]) -
             ring_at(&st->rv_d2[0], st->rv_tap[0][6]);
    wet[1] = ring_at(&st->rv_d1[0], st->rv_tap[0][0]) +
             ring_at(&st->rv_d1[0], st->rv_tap[0][1]) -
             ring_at(&st->rv_ap[0], st->rv_tap[0][2]) +
             ring_at(&st->rv_d2[0], st->rv_tap[0][3]) -
             ring_at(&st->rv_d1[1], st->rv_tap[1][4]) -
             ring_at(&st->rv_ap[1], st->rv_tap[1][5]) -
             ring_at(&st->rv_d2[1], st->rv_tap[1][6]);

    for (s = 0; s < 2; s++) {
        outv[s] = in[s];
        if (st->rv_send != 0) {
            outv[s] = fin(in[s] + st->rv_send * 1.2f * wet[s]);
        }
    }
    *left = outv[0];
    *right = outv[1];
}

static float pitch_ratio(const FxParams *p)
{
    float octaves = p->ps_pitch * p->ps_range;
    float semis;

    if (octaves > 2.0f) {
        octaves = 2.0f;
    }
    if (octaves < -2.0f) {
        octaves = -2.0f;
    }
    if (p->ps_semi) {
        semis = roundf(octaves * 12.0f);
        octaves = semis / 12.0f;
    }
    return powf(2.0f, octaves);
}

/* In-place complex FFT of pv_n points. sign -1 forward, +1 inverse (unscaled). */
static void pv_fft(FxState *st, float *d, int sign)
{
    int n = st->pv_n;
    int i;
    int len;

    for (i = 0; i < n; i++) {
        int j = st->pv_rev[i];

        if (j > i) {
            float tr = d[2 * i];
            float ti = d[2 * i + 1];

            d[2 * i] = d[2 * j];
            d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = tr;
            d[2 * j + 1] = ti;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        int half = len / 2;
        int stride = n / len;

        for (i = 0; i < n; i += len) {
            int j;

            for (j = 0; j < half; j++) {
                float wr = st->pv_cos[j * stride];
                float wi = (float)sign * st->pv_sin[j * stride];
                float *a = &d[2 * (i + j)];
                float *b = &d[2 * (i + j + half)];
                float tr = b[0] * wr - b[1] * wi;
                float ti = b[0] * wi + b[1] * wr;

                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
        }
    }
}

/* Moving average of src over +-hw bins into dst, for bins 0..half. */
static void smooth_bins(const float *src, float *dst, int half, int hw)
{
    int k;

    for (k = 0; k <= half; k++) {
        int lo = k - hw < 0 ? 0 : k - hw;
        int hi = k + hw > half ? half : k + hw;
        float sum = 0;
        int j;

        for (j = lo; j <= hi; j++) {
            sum += src[j];
        }
        dst[k] = sum / (float)(hi - lo + 1);
    }
}

static void pv_frame(FxState *st, PvCh *c, float ratio, int formant)
{
    int n = st->pv_n;
    int half = n / 2;
    int step = st->pv_step;
    float fpb = (float)st->rate / (float)n;
    float expct = 2.0f * PV_PI * (float)step / (float)n;
    float *w = st->pv_work;
    int k;

    for (k = 0; k < n; k++) {
        w[2 * k] = c->in_fifo[k] * st->pv_win[k];
        w[2 * k + 1] = 0;
    }
    pv_fft(st, w, -1);

    for (k = 0; k <= half; k++) {
        float re = w[2 * k];
        float im = w[2 * k + 1];
        float ph = atan2f(im, re);
        float d = ph - c->last_phase[k];
        long q;

        c->last_phase[k] = ph;
        st->pv_mag[k] = 2.0f * sqrtf(re * re + im * im);
        d -= (float)k * expct;
        q = (long)(d / PV_PI);
        if (q >= 0) {
            q += q & 1;
        } else {
            q -= q & 1;
        }
        d -= PV_PI * (float)q;
        d = (float)PV_OSAMP * d / (2.0f * PV_PI);
        st->pv_frq[k] = ((float)k + d) * fpb;
    }

    memset(st->pv_smag, 0, (size_t)(half + 1) * sizeof(float));
    memset(st->pv_sfrq, 0, (size_t)(half + 1) * sizeof(float));
    for (k = 0; k <= half; k++) {
        int idx = (int)((float)k * ratio + 0.5f);

        if (idx <= half) {
            st->pv_smag[idx] += st->pv_mag[k];
            st->pv_sfrq[idx] = st->pv_frq[k] * ratio;
        }
    }

    if (formant) {
        int hw = (int)(300.0f / fpb);

        if (hw < 3) {
            hw = 3;
        }
        smooth_bins(st->pv_mag, st->pv_env, half, hw);
        smooth_bins(st->pv_smag, st->pv_env2, half, hw);
        for (k = 0; k <= half; k++) {
            float g = st->pv_env[k] / (st->pv_env2[k] + 1e-9f);

            if (g > 4.0f) {
                g = 4.0f;
            }
            if (g < 0.25f) {
                g = 0.25f;
            }
            st->pv_smag[k] *= g;
        }
    }

    for (k = 0; k <= half; k++) {
        float d = (st->pv_sfrq[k] - (float)k * fpb) / fpb;
        float ph;

        d = 2.0f * PV_PI * d / (float)PV_OSAMP + (float)k * expct;
        c->sum_phase[k] += d;
        ph = c->sum_phase[k];
        w[2 * k] = st->pv_smag[k] * cosf(ph);
        w[2 * k + 1] = st->pv_smag[k] * sinf(ph);
    }
    for (k = half + 1; k < n; k++) {
        w[2 * k] = 0;
        w[2 * k + 1] = 0;
    }
    pv_fft(st, w, 1);

    for (k = 0; k < n; k++) {
        c->out_accum[k] += 2.0f * st->pv_win[k] * w[2 * k] /
                           ((float)half * (float)PV_OSAMP);
    }
    memcpy(c->out_fifo, c->out_accum, (size_t)step * sizeof(float));
    memmove(c->out_accum, c->out_accum + step, (size_t)n * sizeof(float));
    memset(c->out_accum + n, 0, (size_t)step * sizeof(float));
    memmove(c->in_fifo, c->in_fifo + step, (size_t)st->pv_latency * sizeof(float));
}

static float pitch_channel(FxState *st, int ch, float x, float ratio, int formant)
{
    PvCh *c = &st->pv[ch];
    float y;

    c->in_fifo[c->rover] = x;
    y = c->out_fifo[c->rover - st->pv_latency];
    c->rover++;
    if (c->rover >= st->pv_n) {
        c->rover = st->pv_latency;
        pv_frame(st, c, ratio, formant);
    }
    return fin(y);
}

static void process_pitch(FxState *st, float *left, float *right)
{
    float ratio;
    float target;
    float dry_l = *left;
    float dry_r = *right;
    float wet_l;
    float wet_r;
    int formant;
    int bypass;

    ratio = pitch_ratio(&st->p);
    bypass = !st->ps_on || fabsf(ratio - 1.0f) < 0.0008f;
    if (!bypass && st->ps_warm < st->pv_n * 2) {
        st->ps_warm++;
    }
    /* Stay dry until the vocoder has filled, then fade the shifted sound in. */
    target = (bypass || st->ps_warm < st->pv_n * 2) ? 0 : 1;
    st->ps_mix += 0.002f * (target - st->ps_mix);
    if (st->ps_mix < 0.00005f) {
        st->ps_mix = 0;
    }
    if (st->ps_mix > 0.9999f) {
        st->ps_mix = 1;
    }
    if (st->ps_mix == 0 && bypass) {
        if (st->ps_wet) {
            ps_clear(st);
            st->ps_wet = 0;
        }
        return;
    }
    st->ps_wet = 1;
    formant = st->p.ps_formant && !bypass;
    wet_l = pitch_channel(st, 0, dry_l, bypass ? 1.0f : ratio, formant);
    wet_r = pitch_channel(st, 1, dry_r, bypass ? 1.0f : ratio, formant);
    if (st->ps_mix >= 1.0f) {
        *left = wet_l;
        *right = wet_r;
        return;
    }
    *left = fin(dry_l * (1.0f - st->ps_mix) + wet_l * st->ps_mix);
    *right = fin(dry_r * (1.0f - st->ps_mix) + wet_r * st->ps_mix);
}

static float ap_read(const float *buf, int w, float delay)
{
    float pos = (float)w - 1.0f - delay;
    int i0;
    int i1;
    float frac;

    while (pos < 0) {
        pos += (float)AP_DELAY;
    }
    i0 = (int)pos;
    frac = pos - (float)i0;
    i0 &= AP_DELAY - 1;
    i1 = (i0 + 1) & (AP_DELAY - 1);
    return buf[i0] + (buf[i1] - buf[i0]) * frac;
}

/* First-order allpasses with feedback, mixed with the input. Where the
 * chain turns the phase around, the two cancel, so notches (one per two
 * stages) sweep between low and high, up and down once per period.
 */
static void process_phaser(FxState *st, float *left, float *right)
{
    float fb;
    float blend;
    float gain;
    float lo;
    float hi;
    int stages;
    double sweep;
    float freq;
    float t;
    float a;
    float *io[2];
    int c;
    int k;

    st->ph_mix += 0.003f * ((st->ph_on ? 1.0f : 0.0f) - st->ph_mix);
    if (!st->ph_on && st->ph_mix < 0.0001f) {
        st->ph_mix = 0;
        if (st->ph_live) {
            ph_clear(st);
            st->ph_live = 0;
        }
        return;
    }
    if (st->ph_on && !st->ph_live) {
        ph_clear(st);
        st->ph_live = 1;
    }

    /* Period counts 4-beat measures, snapped to whole beats. */
    sweep = round(st->p.ph_period * 4.0);
    if (sweep < 1) {
        sweep = 1;
    }
    sweep *= st->beat_sec > 0 ? st->beat_sec : 0.5;
    st->ph_phase += 1.0 / (sweep * (double)st->rate);
    if (st->ph_phase >= 1.0) {
        st->ph_phase -= floor(st->ph_phase);
    }
    hi = st->p.ph_high < 0.45f * (float)st->rate ? st->p.ph_high : 0.45f * (float)st->rate;
    lo = st->p.ph_low > 10.0f ? st->p.ph_low : 10.0f;
    if (lo > hi) {
        lo = hi;
    }
    freq = lo * powf(hi / lo, 0.5f - 0.5f * cosf(2.0f * PV_PI * (float)st->ph_phase));
    stages = st->p.ph_stages < 1 ? 1 : st->p.ph_stages > PH_STAGES ? PH_STAGES : st->p.ph_stages;
    fb = st->p.ph_feedback < 0 ? 0 : st->p.ph_feedback > 0.9f ? 0.9f : st->p.ph_feedback;
    blend = clamp_unit(st->p.ph_blend * 0.01f);
    /* Boost makes up for what the notches take out. Turned on, it is there in
     * full from the first sample. Turned off, it fades out with the phasing,
     * so the song does not dip and then come back louder.
     */
    gain = st->p.ph_boost > 0 ? st->p.ph_boost * 0.01f : 0;
    gain = 1.0f + (st->ph_on ? gain : gain * st->ph_mix);
    t = tanf(PV_PI * freq / (float)st->rate);
    a = (t - 1.0f) / (t + 1.0f);

    io[0] = left;
    io[1] = right;
    for (c = 0; c < 2; c++) {
        float x = *io[c];
        float v = x + fb * st->ph_fb[c];

        for (k = 0; k < stages; k++) {
            float y = a * v + st->ph_x[c][k] - a * st->ph_y[c][k];

            st->ph_x[c][k] = v;
            st->ph_y[c][k] = y;
            v = y;
        }
        st->ph_fb[c] = v;
        /* 0.75 keeps the feedback peaks near the input level. */
        v = 0.75f * (x + blend * (v - x));
        *io[c] = fin(gain * (x + st->ph_mix * (v - x)));
    }
}

/* Where the swing is at a phase, -1..1, before distance. */
static float ap_shape(double phase, float smooth)
{
    float hold;
    float boost;
    float pos;

    if (smooth < 0.25f) {
        smooth = 0.25f;
    }
    if (smooth > 0.5f) {
        smooth = 0.5f;
    }
    /* hold is the share of each half swing spent parked at the side. A sine
     * scaled up by 1/cos(pi * hold) and clipped stays at 1 for that long.
     */
    hold = 0.5f - smooth;
    boost = 1.0f / cosf(PV_PI * hold);
    pos = boost * sinf(2.0f * PV_PI * (float)phase);
    if (pos > 1.0f) {
        pos = 1.0f;
    }
    if (pos < -1.0f) {
        pos = -1.0f;
    }
    return pos;
}

/* Turned off while moving out: slow down quickly, stop a little past where
 * it is, and come back to the middle. The turn runs at four times the swing
 * speed; the way back at the swing speed.
 */
static void ap_begin_return(FxState *st, float width, double step)
{
    float s0 = ap_shape(st->ap_phase, st->p.ap_smooth);
    float x0 = width * s0;
    float v0 = width * (ap_shape(st->ap_phase + step, st->p.ap_smooth) - s0);
    float w = 2.0f * PV_PI * (float)step;
    float q = fabsf(v0) / (4.0f * w);
    float amp = sqrtf(x0 * x0 + q * q);

    st->ap_ret_w = w;
    if (amp < 1e-6f) {
        st->ap_ret = 0;
        return;
    }
    st->ap_ret_amp = (x0 != 0 ? x0 : v0) < 0 ? -amp : amp;
    if (q < 1e-9f) {
        /* Parked at the side: start straight back. */
        st->ap_ret = 2;
        st->ap_ret_th = 0;
    } else {
        st->ap_ret = 1;
        st->ap_ret_th = atan2f(fabsf(x0), q);
    }
}

static void process_autopan(FxState *st, float *left, float *right)
{
    double period;
    float width;
    float pos;
    float frac;
    float gl;
    float gr;
    float shrink;
    float delay;
    float l;
    float r;
    double half;
    double step;

    if (!st->ap_live) {
        if (!st->ap_on) {
            return;
        }
        /* Starts at the middle of the swing, heading left or right at
         * random.
         */
        ap_clear(st);
        st->ap_rng ^= st->ap_rng << 13;
        st->ap_rng ^= st->ap_rng >> 17;
        st->ap_rng ^= st->ap_rng << 5;
        st->ap_phase = (st->ap_rng >> 16) & 1 ? 0.5 : 0.0;
        st->ap_mix = 0;
        st->ap_live = 1;
    }
    if (st->ap_on) {
        st->ap_mix += 0.003f * (1.0f - st->ap_mix);
    }

    /* Period counts 4-beat measures, snapped to whole beats. */
    period = round(st->p.ap_period * 4.0);
    if (period < 1) {
        period = 1;
    }
    period *= st->beat_sec > 0 ? st->beat_sec : 0.5;
    step = 1.0 / (period * (double)st->rate);
    width = clamp_unit(st->p.ap_distance * 0.01f) * st->ap_mix;

    if (!st->ap_on && !st->ap_ret &&
        (st->ap_phase < 0.25 || (st->ap_phase >= 0.5 && st->ap_phase < 0.75))) {
        ap_begin_return(st, width, step);
        if (!st->ap_ret) {
            ap_clear(st);
            st->ap_mix = 0;
            st->ap_live = 0;
            return;
        }
    }

    if (st->ap_ret == 1) {
        st->ap_ret_th += 4.0f * st->ap_ret_w;
        if (st->ap_ret_th >= 0.5f * PV_PI) {
            st->ap_ret = 2;
            st->ap_ret_th = 0;
        }
        pos = st->ap_ret_amp * (st->ap_ret == 1 ? sinf(st->ap_ret_th) : 1.0f);
    } else if (st->ap_ret == 2) {
        st->ap_ret_th += st->ap_ret_w;
        if (st->ap_ret_th >= 0.5f * PV_PI) {
            /* Back at the middle. Off stops here; on again carries on
             * swinging toward the other side.
             */
            if (!st->ap_on) {
                ap_clear(st);
                st->ap_mix = 0;
                st->ap_live = 0;
                return;
            }
            st->ap_phase = st->ap_ret_amp > 0 ? 0.5 : 0.0;
            st->ap_ret = 0;
            pos = 0;
        } else {
            pos = st->ap_ret_amp * cosf(st->ap_ret_th);
        }
    } else {
        half = st->ap_phase < 0.5 ? 0.5 : 1.0;
        st->ap_phase += step;
        if (!st->ap_on && st->ap_phase >= half) {
            /* Turned off on the way in: the swing ran on to the middle,
             * where the song is back as it was, so stop here.
             */
            ap_clear(st);
            st->ap_mix = 0;
            st->ap_live = 0;
            return;
        }
        if (st->ap_phase >= 1.0) {
            st->ap_phase -= floor(st->ap_phase);
        }
        pos = width * ap_shape(st->ap_phase, st->p.ap_smooth);
    }

    /* Equal power, so the song is as loud at the side as in the middle,
     * trimmed a little at the sides so peaks do not clip.
     */
    frac = 0.5f * (pos + 1.0f);
    shrink = 1.0f - 0.15f * fabsf(pos);
    gl = 1.41421356f * sinf(frac * 0.5f * PV_PI) * shrink;
    gr = 1.41421356f * cosf(frac * 0.5f * PV_PI) * shrink;

    /* The far side also arrives up to 1 ms late, which the ear hears as
     * direction.
     */
    st->ap_buf[0][st->ap_w] = *left;
    st->ap_buf[1][st->ap_w] = *right;
    st->ap_w = (st->ap_w + 1) & (AP_DELAY - 1);
    delay = 0.001f * (float)st->rate * fabsf(pos);
    if (delay > (float)(AP_DELAY - 4)) {
        delay = (float)(AP_DELAY - 4);
    }
    l = ap_read(st->ap_buf[0], st->ap_w, pos < 0 ? delay : 0);
    r = ap_read(st->ap_buf[1], st->ap_w, pos > 0 ? delay : 0);
    *left = fin(l * gl);
    *right = fin(r * gr);
}

/* One up-and-down of the delay, in samples. Period counts 4-beat measures
 * and snaps to whole beats, the same way autopan and the phaser do.
 */
static double mod_step(const FxState *st, float measures)
{
    double beats = round((double)measures * 4.0);

    if (beats < 1) {
        beats = 1;
    }
    beats *= st->beat_sec > 0 ? (double)st->beat_sec : 0.5;
    return 1.0 / (beats * (double)st->rate);
}

static float mod_lfo(double phase, float offset)
{
    float p = (float)phase + offset;

    p -= floorf(p);
    return sinf(2.0f * PV_PI * p);
}

/* delay is in samples behind the write head. Linear interpolation. */
static float line_read(const float *buf, int len, int w, float delay)
{
    int back;
    int i0;
    int i1;
    float frac;

    if (delay < 1.0f) {
        delay = 1.0f;
    }
    if (delay > (float)(len - 3)) {
        delay = (float)(len - 3);
    }
    back = (int)delay;
    frac = delay - (float)back;
    i0 = w - back;
    i1 = i0 - 1;
    while (i0 < 0) {
        i0 += len;
    }
    while (i1 < 0) {
        i1 += len;
    }
    if (i0 >= len) {
        i0 %= len;
    }
    if (i1 >= len) {
        i1 %= len;
    }
    return buf[i0] * (1.0f - frac) + buf[i1] * frac;
}

static void line_store(float *buf, int w, float x)
{
    buf[w] = x;
}

static void line_advance(int *w, int len)
{
    *w += 1;
    if (*w >= len) {
        *w = 0;
    }
}

/* Bends peaks above full scale instead of letting the feedback run away. */
static float soft_clip(float x)
{
    float a = fabsf(x);
    float y;

    if (a <= 1.0f) {
        return x;
    }
    y = 1.0f + (a - 1.0f) / (1.0f + (a - 1.0f));
    return x < 0 ? -y : y;
}

static void mod_ramp(float *mix, int on)
{
    *mix += 0.003f * ((on ? 1.0f : 0.0f) - *mix);
    if (*mix < 0.0001f) {
        *mix = 0;
    }
    if (*mix > 0.9999f) {
        *mix = 1;
    }
}

/* Voice v of n sits this far from the center delay, in milliseconds, so
 * the copies arrive separately instead of piling up into one.
 */
static float chorus_delay_ms(float center, float depth, int voice, int voices, float lfo)
{
    float spread = 0;
    float d;

    if (voices > 1) {
        spread = center * 0.12f * ((float)voice - 0.5f * (float)(voices - 1));
    }
    d = center + spread + depth * lfo;
    if (d < 0.2f) {
        d = 0.2f;
    }
    if (d > 60.0f) {
        d = 60.0f;
    }
    return d;
}

/* The right speaker's oscillator is a quarter turn ahead of the left, so
 * the copies drift apart and the one part sounds like several players.
 */
static void process_chorus(FxState *st, float *left, float *right)
{
    float amount;
    float center;
    float depth;
    int voices;
    float *io[2];
    int c;
    int v;

    mod_ramp(&st->ch_mix, st->ch_on);
    if (!st->ch_on && st->ch_mix == 0) {
        if (st->ch_live) {
            ch_clear(st);
            st->ch_live = 0;
        }
        return;
    }
    if (st->ch_on && !st->ch_live) {
        ch_clear(st);
        st->ch_live = 1;
    }
    amount = clamp_unit(st->p.ch_mix * 0.01f) * st->ch_mix;
    if (amount == 0) {
        return;
    }
    center = st->p.ch_delay;
    if (center < 5.0f) {
        center = 5.0f;
    }
    if (center > 40.0f) {
        center = 40.0f;
    }
    depth = st->p.ch_depth;
    if (depth < 0) {
        depth = 0;
    }
    if (depth > 10.0f) {
        depth = 10.0f;
    }
    voices = st->p.ch_voices < 1 ? 1 : st->p.ch_voices > 4 ? 4 : st->p.ch_voices;
    io[0] = left;
    io[1] = right;
    for (c = 0; c < 2; c++) {
        float dry = *io[c];
        float wet = 0;
        float side = c ? 0.25f : 0;

        for (v = 0; v < voices; v++) {
            float lfo = mod_lfo(st->ch_phase, side + (float)v / (float)voices);
            float ms = chorus_delay_ms(center, depth, v, voices, lfo);
            float samples = ms * 0.001f * (float)st->rate;

            wet += line_read(st->ch_buf[c], st->ch_len, st->ch_w, samples);
        }
        wet /= (float)voices;
        line_store(st->ch_buf[c], st->ch_w, dry);
        *io[c] = fin(dry * (1.0f - amount) + wet * amount);
    }
    line_advance(&st->ch_w, st->ch_len);
    st->ch_phase += mod_step(st, st->p.ch_period);
    if (st->ch_phase >= 1.0) {
        st->ch_phase -= floor(st->ch_phase);
    }
}

/* Same mix as chorus, but the delay is only a millisecond or two, so the
 * copy lines up against the original and carves moving notches. Feedback
 * makes those notches ring, which is the jet-plane sweep.
 */
static void process_flanger(FxState *st, float *left, float *right)
{
    float amount;
    float center;
    float depth;
    float fb;
    float *io[2];
    int c;

    mod_ramp(&st->fl_mix, st->fl_on);
    if (!st->fl_on && st->fl_mix == 0) {
        if (st->fl_live) {
            fl_clear(st);
            st->fl_live = 0;
        }
        return;
    }
    if (st->fl_on && !st->fl_live) {
        fl_clear(st);
        st->fl_live = 1;
    }
    amount = clamp_unit(st->p.fl_mix * 0.01f) * st->fl_mix;
    center = st->p.fl_delay;
    if (center < 0.2f) {
        center = 0.2f;
    }
    if (center > 5.0f) {
        center = 5.0f;
    }
    depth = st->p.fl_depth;
    if (depth < 0) {
        depth = 0;
    }
    if (depth > 4.0f) {
        depth = 4.0f;
    }
    fb = st->p.fl_feedback;
    if (fb < 0) {
        fb = 0;
    }
    if (fb > 0.9f) {
        fb = 0.9f;
    }
    io[0] = left;
    io[1] = right;
    for (c = 0; c < 2; c++) {
        float dry = *io[c];
        float lfo = mod_lfo(st->fl_phase, c ? 0.25f : 0);
        float ms = center + depth * lfo;
        float delayed;

        if (ms < 0.05f) {
            ms = 0.05f;
        }
        if (ms > 12.0f) {
            ms = 12.0f;
        }
        delayed = line_read(st->fl_buf[c], st->fl_len, st->fl_w,
                            ms * 0.001f * (float)st->rate);
        line_store(st->fl_buf[c], st->fl_w, soft_clip(dry + fb * delayed));
        if (amount != 0) {
            *io[c] = fin(dry * (1.0f - amount) + delayed * amount);
        }
    }
    line_advance(&st->fl_w, st->fl_len);
    st->fl_phase += mod_step(st, st->p.fl_period);
    if (st->fl_phase >= 1.0) {
        st->fl_phase -= floor(st->fl_phase);
    }
}

/* RBJ low-pass biquad for cutoff fc and quality q. */
static void lowpass_coef(float *c, float fc, float q, unsigned rate)
{
    float w0 = 2.0f * PV_PI * fc / (float)rate;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;

    c[0] = 0.5f * (1.0f - cw) / a0;
    c[1] = (1.0f - cw) / a0;
    c[2] = c[0];
    c[3] = -2.0f * cw / a0;
    c[4] = (1.0f - alpha) / a0;
}

static float biquad(const float *c, float *z, float x)
{
    float y = c[0] * x + z[0];

    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}

/* A slowly moving delay bends the pitch a few cents up and down. Two
 * low-passes in a row take the highs out steeply (24 dB per octave); the
 * first has a resonant peak that gives the hollow, boomy tone. The right
 * speaker's wobble runs a little behind the left, which widens it.
 */
static void process_underwater(FxState *st, float *left, float *right)
{
    float amount;
    float fc;
    float q;
    float wobble;
    float gain;
    float *io[2];
    int c;

    mod_ramp(&st->uw_mix, st->uw_on);
    if (!st->uw_on && st->uw_mix == 0) {
        if (st->uw_live) {
            uw_clear(st);
            st->uw_live = 0;
        }
        return;
    }
    if (st->uw_on && !st->uw_live) {
        uw_clear(st);
        st->uw_live = 1;
    }
    fc = st->p.uw_cutoff;
    if (fc < 100.0f) {
        fc = 100.0f;
    }
    if (fc > 0.45f * (float)st->rate) {
        fc = 0.45f * (float)st->rate;
    }
    q = 0.707f + 4.0f * clamp_unit(st->p.uw_res);
    if (fc != st->uw_fc || q != st->uw_q) {
        lowpass_coef(st->uw_coef[0], fc, q, st->rate);
        lowpass_coef(st->uw_coef[1], fc, 0.707f, st->rate);
        st->uw_fc = fc;
        st->uw_q = q;
    }
    wobble = st->p.uw_wobble;
    if (wobble < 0) {
        wobble = 0;
    }
    if (wobble > 5.0f) {
        wobble = 5.0f;
    }
    gain = 1.0f + (st->p.uw_boost > 0 ? st->p.uw_boost * 0.01f : 0);
    amount = clamp_unit(st->p.uw_mix * 0.01f) * st->uw_mix;
    io[0] = left;
    io[1] = right;
    for (c = 0; c < 2; c++) {
        float dry = *io[c];
        float lfo = mod_lfo(st->uw_phase, c ? 0.15f : 0);
        float ms = 1.0f + wobble * (1.0f + lfo);
        float wet;

        line_store(st->uw_buf[c], st->uw_w, dry);
        wet = line_read(st->uw_buf[c], st->uw_len, st->uw_w, ms * 0.001f * (float)st->rate);
        wet = biquad(st->uw_coef[0], st->uw_z[c][0], wet);
        wet = biquad(st->uw_coef[1], st->uw_z[c][1], wet);
        if (amount != 0) {
            *io[c] = fin(dry * (1.0f - amount) + soft_clip(wet * gain) * amount);
        }
    }
    line_advance(&st->uw_w, st->uw_len);
    st->uw_phase += mod_step(st, st->p.uw_period);
    if (st->uw_phase >= 1.0) {
        st->uw_phase -= floor(st->uw_phase);
    }
}

static unsigned lz_rand(FxState *st)
{
    unsigned x = st->lz_rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (!x) {
        x = 1;
    }
    st->lz_rng = x;
    return x;
}

/* (0, 1). Never quite 0, so logf of it is safe. */
static float lz_unit(FxState *st)
{
    float u = (lz_rand(st) >> 8) * (1.0f / 16777216.0f);

    return u < 1.0e-4f ? 1.0e-4f : u;
}

static int lz_alloc(FxState *st)
{
    int i;
    int quiet = 0;

    for (i = 0; i < LZ_VOICES; i++) {
        if (st->lz[i].amp < 1.0e-5f) {
            return i;
        }
        if (st->lz[i].amp < st->lz[quiet].amp) {
            quiet = i;
        }
    }
    return quiet;
}

/* A new zap. Most start near the middle of the range; fewer at either
 * end. Lower ones are a little longer, louder, and fall further.
 * One tone only. A noise burst here is what turns the bed into rain. */
static Laser *lz_spawn(FxState *st, float low, float high, float length_ms)
{
    Laser *v = &st->lz[lz_alloc(st)];
    float span = 0.5f * (lz_unit(st) + lz_unit(st));
    float hz;
    float place;
    float size;
    float ratio;
    float ms;
    float nsmpl;
    float drop;
    float nattack;
    float pan;

    if (!(high > low)) {
        hz = low;
        place = 0;
    } else {
        hz = low * powf(high / low, span);
        place = (hz - low) / (high - low);
    }
    size = 1.0f - place;
    /* A low zap falls about an octave. A high one falls about a fifth.
     * The fall lasts as long as the note is loud. */
    ratio = 0.42f + 0.28f * place;
    ms = length_ms * (0.85f + 0.3f * size) * (0.85f + 0.3f * lz_unit(st));
    nsmpl = ms * 0.001f * (float)st->rate;
    if (nsmpl < 8.0f) {
        nsmpl = 8.0f;
    }
    if (hz < 40.0f) {
        hz = 40.0f;
    }
    if (hz > 0.45f * (float)st->rate) {
        hz = 0.45f * (float)st->rate;
    }
    memset(v, 0, sizeof *v);
    v->freq = 2.0f * PV_PI * hz / (float)st->rate;
    v->target = v->freq * ratio;
    if (v->target < 2.0f * PV_PI * 40.0f / (float)st->rate) {
        v->target = 2.0f * PV_PI * 40.0f / (float)st->rate;
    }
    drop = nsmpl * 0.80f;
    if (drop < 8.0f) {
        drop = 8.0f;
    }
    v->settle = 1.0f - powf(0.08f, 1.0f / drop);
    v->amp = (0.20f + 0.12f * size) * (0.85f + 0.3f * lz_unit(st));
    /* Stay loud long enough to hear the fall, then get out of the way. */
    v->decay = powf(0.04f, 1.0f / nsmpl);
    nattack = 0.016f * (float)st->rate;
    if (nattack < 1.0f) {
        nattack = 1.0f;
    }
    v->attack_step = 1.0f / nattack;
    v->phase = lz_unit(st) * 2.0f * PV_PI;
    pan = 0.42f + 0.16f * lz_unit(st);
    v->gain_l = sqrtf(1.0f - pan);
    v->gain_r = sqrtf(pan);
    return v;
}

static float lz_gap(FxState *st, float per_sec)
{
    float gap = -logf(lz_unit(st)) / per_sec * (float)st->rate;

    return gap < 1.0f ? 1.0f : gap;
}

/* Adds the zaps. A level of 0, or the effect settled in the off state,
 * leaves the song sample untouched. Turning it off does not cut a zap
 * that is already sounding: that one finishes, and so does a second zap
 * that was about to follow it. No new zap is started. */
static void process_lasers(FxState *st, float *left, float *right)
{
    float per_sec;
    float low;
    float high;
    float length;
    float level;
    float gain;
    float sum_l = 0;
    float sum_r = 0;
    int i;

    if (st->lz_on && !st->lz_live) {
        lz_clear(st);
        st->lz_live = 1;
    }
    if (!st->lz_on && !lz_sounding(st)) {
        if (st->lz_live || st->lz_mix != 0) {
            lz_clear(st);
            st->lz_live = 0;
            st->lz_mix = 0;
        }
        return;
    }
    /* Fade in when it is turned on. While it is turned off, hold the level
     * so the zap that is already sounding does not fade out. */
    if (st->lz_on) {
        mod_ramp(&st->lz_mix, 1);
    }
    per_sec = st->p.lz_rate;
    if (per_sec < 0.2f) {
        per_sec = 0.2f;
    }
    if (per_sec > 20.0f) {
        per_sec = 20.0f;
    }
    low = st->p.lz_low;
    high = st->p.lz_high;
    if (low < 80.0f) {
        low = 80.0f;
    }
    if (high < 80.0f) {
        high = 80.0f;
    }
    if (low > high) {
        float swap = low;

        low = high;
        high = swap;
    }
    if (high > 0.45f * (float)st->rate) {
        high = 0.45f * (float)st->rate;
    }
    if (low > high) {
        low = high;
    }
    length = st->p.lz_length;
    if (length < 30.0f) {
        length = 30.0f;
    }
    if (length > 400.0f) {
        length = 400.0f;
    }
    level = st->p.lz_level * 0.01f;
    if (level < 0) {
        level = 0;
    }
    if (level > 1.0f) {
        level = 1.0f;
    }
    if (st->lz_on || st->lz_follow) {
        st->lz_wait -= 1.0f;
        if (st->lz_wait <= 0) {
            lz_spawn(st, low, high, length);
            if (!st->lz_on) {
                /* That was the second zap. Do not start another. */
                st->lz_pair = 0;
                st->lz_follow = 0;
                st->lz_wait = 1.0f;
            } else if (st->lz_pair) {
                st->lz_pair = 0;
                /* The second of the pair. Far enough out that a pitch
                 * check of the first zap does not hear it. */
                st->lz_follow = 1;
                st->lz_wait = (0.09f + 0.08f * lz_unit(st)) * (float)st->rate;
            } else {
                st->lz_follow = 0;
                if (lz_unit(st) < 0.5f) {
                    st->lz_pair = 1;
                }
                st->lz_wait = lz_gap(st, per_sec);
            }
        }
    }
    for (i = 0; i < LZ_VOICES; i++) {
        Laser *v = &st->lz[i];
        float env;
        float s;

        if (v->amp < 1.0e-5f) {
            v->amp = 0;
            continue;
        }
        if (v->attack < 1.0f) {
            v->attack += v->attack_step;
            if (v->attack > 1.0f) {
                v->attack = 1.0f;
            }
        }
        /* A rounded onset. A sharp tick is a raindrop. */
        env = v->amp * sinf(v->attack * 0.5f * PV_PI);
        s = sinf(v->phase) * env;
        sum_l += s * v->gain_l;
        sum_r += s * v->gain_r;
        v->phase += v->freq;
        if (v->phase >= 2.0f * PV_PI) {
            v->phase -= 2.0f * PV_PI;
        }
        v->freq += (v->target - v->freq) * v->settle;
        v->amp *= v->decay;
    }
    gain = level * st->lz_mix;
    if (gain != 0) {
        *left = fin(*left + sum_l * gain);
        *right = fin(*right + sum_r * gain);
    }
}

void fx_process(FxState *st, float *left, float *right)
{
    if (!st || !left || !right) {
        return;
    }
    process_reverb(st, left, right);
    process_pitch(st, left, right);
    process_chorus(st, left, right);
    process_flanger(st, left, right);
    process_underwater(st, left, right);
    process_phaser(st, left, right);
    process_autopan(st, left, right);
    process_lasers(st, left, right);
}

static int near_same(const float *a, const float *b, int n, float tol)
{
    int i;

    for (i = 0; i < n; i++) {
        if (fabsf(a[i] - b[i]) > tol) {
            return 0;
        }
    }
    return 1;
}

static int crossings(const float *x, int n)
{
    int i;
    int c = 0;

    for (i = 1; i < n; i++) {
        if ((x[i - 1] <= 0 && x[i] > 0) || (x[i - 1] >= 0 && x[i] < 0)) {
            c++;
        }
    }
    return c;
}

/* Lag, in samples, of the strongest repeating tone in x[0..n).
 * A smooth noise correlates most at the shortest lag, so only a local
 * peak counts, and it has to be a real share of the window's energy. */
static int main_period(const float *x, int n, int min_lag, int max_lag)
{
    int lag;
    int best = 0;
    double best_c = 0;
    double r0 = 0;
    double prev = 0;
    double older = 0;
    int i;

    if (n < 8) {
        return 0;
    }
    if (max_lag > n / 2) {
        max_lag = n / 2;
    }
    for (i = 0; i < n; i++) {
        r0 += (double)x[i] * x[i];
    }
    if (r0 < 1.0e-8) {
        return 0;
    }
    for (lag = min_lag; lag <= max_lag; lag++) {
        double c = 0;
        int m = n - lag;

        for (i = 0; i < m; i++) {
            c += (double)x[i] * x[i + lag];
        }
        c /= (double)m;
        if (lag > min_lag + 1 && prev >= older && prev >= c && prev > best_c &&
            prev > 0.15 * r0 / (double)n) {
            best_c = prev;
            best = lag - 1;
        }
        older = prev;
        prev = c;
    }
    return best;
}

/* RMS of the last half second of a tone through underwater, fully on. */
static float underwater_rms(FxState *st, const FxParams *p, float hz)
{
    double sum = 0;
    int i;

    fx_setup(st, p);
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 1, 0);
    st->uw_mix = 1;
    for (i = 0; i < 44100; i++) {
        float l = 0.5f * sinf(2.0f * PV_PI * hz * (float)i / 44100.0f);
        float r = l;

        fx_process(st, &l, &r);
        if (l != l || l > 2.0f || l < -2.0f) {
            return -1;
        }
        if (i >= 22050) {
            sum += (double)l * l;
        }
    }
    return (float)sqrt(sum / 22050.0);
}

/* Underwater keeps the low end, takes the highs out steeply, and leaves the
 * song alone when it is off. */
static int underwater_self_test(void)
{
    FxState *st = fx_new(44100);
    FxParams p;
    float low;
    float high;
    int rc = -1;
    int i;

    if (!st) {
        log_error("fx", "out of memory");
        return -1;
    }
    fx_defaults(&p);
    p.uw_wobble = 0;
    p.uw_res = 0;
    p.uw_boost = 0;
    low = underwater_rms(st, &p, 100.0f);
    high = underwater_rms(st, &p, 5000.0f);
    /* A 0.5 sine has an RMS of 0.354. Two octaves and more above 500 Hz,
     * 24 dB per octave leaves well under 1%. */
    if (low < 0.3f || low > 0.4f || high < 0 || high > 0.004f) {
        log_error("fx", "underwater passed %.4f at 100 Hz and %.4f at 5 kHz", (double)low,
                  (double)high);
        goto done;
    }
    /* Boost 20 makes it exactly 1.2 times as loud. */
    p.uw_boost = 20;
    high = underwater_rms(st, &p, 100.0f);
    if (high < low * 1.19f || high > low * 1.21f) {
        log_error("fx", "underwater boost 20 changed the level %.3f times", (double)(high / low));
        goto done;
    }
    fx_defaults(&p);
    if (underwater_rms(st, &p, 300.0f) < 0) {
        log_error("fx", "underwater with its defaults ran away");
        goto done;
    }
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 1000; i++) {
        float x = 0.5f * sinf(2.0f * PV_PI * 5000.0f * (float)i / 44100.0f);
        float l = x;
        float r = x;

        fx_process(st, &l, &r);
        if (l != x || r != x) {
            log_error("fx", "underwater off changed the song");
            goto done;
        }
    }
    rc = 0;
done:
    fx_free(st);
    return rc;
}

/* Lasers are added on top. Off, or level 0, leaves the song alone. On,
 * silence becomes a bed of short zaps, a louder level is louder, the bed
 * stays under 2500 Hz, and one zap falls in pitch.
 */
static int lasers_self_test(void)
{
    FxState *st = fx_new(44100);
    FxParams p;
    float *buf = NULL;
    float loud;
    float quiet;
    double sum;
    double mean;
    int i;
    int rc = -1;

    if (!st) {
        log_error("fx", "out of memory");
        return -1;
    }
    fx_defaults(&p);
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 1000; i++) {
        float x = 0.25f;
        float l = x;
        float r = x;

        fx_process(st, &l, &r);
        if (l != x || r != x) {
            log_error("fx", "lasers off changed the song");
            goto done;
        }
    }
    p.lz_level = 0;
    fx_setup(st, &p);
    st->lz_rng = 1;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    for (i = 0; i < 4000; i++) {
        float x = 0.25f;
        float l = x;
        float r = x;

        fx_process(st, &l, &r);
        if (l != x || r != x) {
            log_error("fx", "lasers at level 0 changed the song");
            goto done;
        }
    }

    fx_defaults(&p);
    p.lz_rate = 8;
    p.lz_level = 100;
    fx_setup(st, &p);
    st->lz_rng = 0x12345678u;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    sum = 0;
    for (i = 0; i < 44100; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
        if (l != l || r != r || l > 2.0f || l < -2.0f || r > 2.0f || r < -2.0f) {
            log_error("fx", "lasers ran away to %.3f", (double)l);
            goto done;
        }
        sum += (double)l * l + (double)r * r;
    }
    loud = (float)sqrt(sum / (44100.0 * 2.0));
    if (loud < 0.02f) {
        log_error("fx", "lasers on silence were only %.4f rms", (double)loud);
        goto done;
    }

    p.lz_level = 25;
    fx_setup(st, &p);
    st->lz_rng = 0x12345678u;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    sum = 0;
    for (i = 0; i < 44100; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
        sum += (double)l * l + (double)r * r;
    }
    quiet = (float)sqrt(sum / (44100.0 * 2.0));
    if (loud < quiet * 2.5f) {
        log_error("fx", "lasers level 100 was %.4f rms and level 25 was %.4f", (double)loud,
                  (double)quiet);
        goto done;
    }

    fx_defaults(&p);
    p.lz_rate = 8;
    p.lz_level = 80;
    fx_setup(st, &p);
    st->lz_rng = 0x89abcdefu;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    sum = 0;
    mean = 0;
    for (i = 0; i < 44100; i++) {
        float l = 0.2f;
        float r = 0.2f;

        fx_process(st, &l, &r);
        mean += l;
        sum += (double)(l - 0.2f) * (l - 0.2f);
    }
    mean /= 44100.0;
    if (mean < 0.17 || mean > 0.23 || sum / 44100.0 < 1.0e-4) {
        log_error("fx", "lasers overlay mean %.4f energy %.6f", mean, sum / 44100.0);
        goto done;
    }

    p.lz_rate = 0.2f;
    p.lz_low = 800;
    p.lz_high = 800;
    p.lz_length = 180;
    p.lz_level = 100;
    fx_setup(st, &p);
    st->lz_rng = 0x12345678u;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    buf = calloc(20000, sizeof(float));
    if (!buf) {
        log_error("fx", "out of memory");
        goto done;
    }
    for (i = 0; i < 20000; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
        buf[i] = l;
    }
    {
        /* After the rounded onset, then further down the same fall. */
        int early = main_period(buf + 800, 700, 16, 700);
        int late = main_period(buf + 2600, 1000, 16, 700);

        if (early < 16 || late < early + early / 5) {
            log_error("fx", "laser pitch did not fall (period %d then %d)", early, late);
            goto done;
        }
    }
    fx_defaults(&p);
    fx_setup(st, &p);
    st->lz_rng = 0x12345678u;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    {
        /* Almost all of the energy stays under 2500 Hz. */
        double low_e = 0;
        double high_e = 0;
        float z = 0;
        float a = 1.0f - expf(-2.0f * PV_PI * 2500.0f / 44100.0f);

        for (i = 0; i < 44100; i++) {
            float l = 0;
            float r = 0;
            float hi;

            fx_process(st, &l, &r);
            z += (l - z) * a;
            low_e += (double)z * z;
            hi = l - z;
            high_e += (double)hi * hi;
        }
        if (high_e <= 0 || low_e < high_e * 2.0) {
            log_error("fx", "lasers were too bright (low %.3f high %.3f)", low_e, high_e);
            goto done;
        }
    }

    fx_defaults(&p);
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    for (i = 0; i < 1000; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
    }
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    {
        /* The zap that was already sounding keeps going. */
        double tail = 0;

        for (i = 0; i < 8000; i++) {
            float l = 0;
            float r = 0;

            fx_process(st, &l, &r);
            tail += (double)l * l;
        }
        if (tail < 1.0e-3) {
            log_error("fx", "turning lasers off cut a zap that was still sounding");
            goto done;
        }
    }
    /* Long enough for that zap, and a second one that was about to follow, to end. */
    for (i = 0; i < 44100 * 3; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
    }
    for (i = 0; i < 200; i++) {
        float x = 0.3f * sinf(2.0f * PV_PI * 440.0f * (float)i / 44100.0f);
        float l = x;
        float r = x;

        fx_process(st, &l, &r);
        if (l != x || r != x) {
            log_error("fx", "lasers stayed on after its zaps finished");
            goto done;
        }
    }
    /* In the gap, with nothing sounding, turning it off adds nothing. */
    p.lz_rate = 0.2f;
    p.lz_length = 40;
    p.lz_level = 100;
    fx_setup(st, &p);
    st->lz_rng = 0x12345678u;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    for (i = 0; i < 44100 * 8 && lz_sounding(st); i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
    }
    if (lz_sounding(st)) {
        log_error("fx", "lasers never left a gap to turn off in");
        goto done;
    }
    /* A zap that is only scheduled, not sounding and not the second of a
     * pair, must not start after the effect is turned off. */
    st->lz_wait = 1.0f;
    st->lz_pair = 1;
    st->lz_follow = 0;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 1000; i++) {
        float x = 0.25f;
        float l = x;
        float r = x;

        fx_process(st, &l, &r);
        if (l != x || r != x) {
            log_error("fx", "turning lasers off in a gap started another zap");
            goto done;
        }
    }
    fx_defaults(&p);
    fx_setup(st, &p);
    st->lz_rng = 0x89abcdefu;
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 1);
    for (i = 0; i < 44100 * 4 && !(st->lz_follow && st->lz_wait > 400.0f); i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
    }
    if (!st->lz_follow) {
        log_error("fx", "lasers never lined up a second zap");
        goto done;
    }
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 44100 && st->lz_follow; i++) {
        float l = 0;
        float r = 0;

        fx_process(st, &l, &r);
    }
    if (st->lz_follow || !lz_sounding(st)) {
        log_error("fx", "the second zap did not finish starting after lasers turned off");
        goto done;
    }
    rc = 0;
done:
    free(buf);
    fx_free(st);
    return rc;
}

int fx_self_test(void)
{
    enum { N = 22050 };
    FxState *st = fx_new(44100);
    FxParams p;
    float *dry;
    float *wet;
    float left;
    float right;
    float energy = 0;
    float peak = 0;
    int i;
    int in_cross;
    int out_cross;
    int rc = -1;

    if (!st) {
        log_error("fx", "out of memory");
        return -1;
    }
    dry = calloc((size_t)N, sizeof(float));
    wet = calloc((size_t)N, sizeof(float));
    if (!dry || !wet) {
        log_error("fx", "out of memory");
        goto done;
    }
    for (i = 0; i < N; i++) {
        dry[i] = sinf(2.0f * (float)M_PI * 220.0f * (float)i / 44100.0f);
    }

    fx_defaults(&p);
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 200; i++) {
        left = dry[i];
        right = dry[i] * 0.5f;
        fx_process(st, &left, &right);
        wet[i] = left;
    }
    if (!near_same(dry, wet, 200, 0.00001f)) {
        log_error("fx", "effects off changed the sound");
        goto done;
    }

    p.reverb_on = 1;
    p.rv_send = 0;
    fx_setup(st, &p);
    fx_set_on(st, 1, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 200; i++) {
        left = dry[i];
        right = dry[i];
        fx_process(st, &left, &right);
        wet[i] = left;
    }
    if (!near_same(dry, wet, 200, 0.00001f)) {
        log_error("fx", "reverb with send 0 was not dry");
        goto done;
    }

    p.rv_send = 1;
    p.rv_decay = 0.8f;
    fx_setup(st, &p);
    fx_set_on(st, 1, 0, 0, 0, 0, 0, 0, 0);
    left = 1;
    right = 1;
    fx_process(st, &left, &right);
    for (i = 1; i < 8000; i++) {
        left = 0;
        right = 0;
        fx_process(st, &left, &right);
        energy += fabsf(left);
        if (fabsf(left) > peak) {
            peak = fabsf(left);
        }
        if (left != left || right != right) {
            log_error("fx", "reverb produced a non-number");
            goto done;
        }
    }
    if (energy < 0.05f || peak > 4.0f) {
        log_error("fx", "reverb tail energy %.4f peak %.3f", energy, peak);
        goto done;
    }

    fx_defaults(&p);
    p.pitch_on = 1;
    p.ps_pitch = 0;
    fx_setup(st, &p);
    fx_set_on(st, 0, 1, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 300; i++) {
        left = dry[i];
        right = dry[i];
        fx_process(st, &left, &right);
        wet[i] = left;
    }
    if (!near_same(dry, wet, 300, 0.00001f)) {
        log_error("fx", "pitch at 0 was not a bypass");
        goto done;
    }

    p.ps_pitch = 1;
    p.ps_range = 1;
    p.ps_semi = 0;
    p.ps_formant = 0;
    fx_setup(st, &p);
    fx_set_on(st, 0, 1, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < N; i++) {
        left = dry[i];
        right = dry[i];
        fx_process(st, &left, &right);
        wet[i] = left;
        if (left != left || fabsf(left) > 4.0f) {
            log_error("fx", "chipmunk produced %f", left);
            goto done;
        }
    }
    in_cross = crossings(dry + 8000, N - 8000);
    out_cross = crossings(wet + 8000, N - 8000);
    if (out_cross < in_cross * 3 / 2 || out_cross > in_cross * 5 / 2) {
        log_error("fx", "octave up crossings %d, dry %d", out_cross, in_cross);
        goto done;
    }

    /* Distance 100%, one beat per swing at 120 BPM: half a second. A quarter of
     * the way through a swing the song is all on the left, three quarters
     * of the way all on the right.
     */
    fx_defaults(&p);
    p.autopan_on = 1;
    p.ap_distance = 100;
    p.ap_period = 0.25f;
    fx_setup(st, &p);
    st->ap_rng = 0;
    fx_set_on(st, 0, 0, 1, 0, 0, 0, 0, 0);
    fx_set_beat(st, 0.5f);
    {
        float at_left[2] = {0, 0};
        float at_right[2] = {0, 0};

        for (i = 0; i < 22050 * 2; i++) {
            left = 0.5f;
            right = 0.5f;
            fx_process(st, &left, &right);
            if (left != left || right != right || fabsf(left) > 1.0f || fabsf(right) > 1.0f) {
                log_error("fx", "autopan produced %f %f", left, right);
                goto done;
            }
            if (i == 22050 + 5512) {
                at_left[0] = left;
                at_left[1] = right;
            }
            if (i == 22050 + 16537) {
                at_right[0] = left;
                at_right[1] = right;
            }
        }
        if (at_left[0] < 0.55f || at_left[1] > 0.01f ||
            at_right[1] < 0.55f || at_right[0] > 0.01f) {
            log_error("fx", "autopan did not swing with the beat (%.3f %.3f, %.3f %.3f)",
                      at_left[0], at_left[1], at_right[0], at_right[1]);
            goto done;
        }
    }

    /* Turned off at full left, it keeps swinging back to the middle, then
     * passes the song through untouched.
     */
    fx_setup(st, &p);
    st->ap_rng = 0;
    fx_set_on(st, 0, 0, 1, 0, 0, 0, 0, 0);
    for (i = 0; i < 5512; i++) {
        left = 0.5f;
        right = 0.5f;
        fx_process(st, &left, &right);
    }
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < 6000; i++) {
        left = 0.5f;
        right = 0.5f;
        fx_process(st, &left, &right);
        if (i == 2000 && right > 0.2f) {
            log_error("fx", "autopan snapped back when turned off (%.3f %.3f)", left, right);
            goto done;
        }
        if (i >= 5600 && (left != 0.5f || right != 0.5f)) {
            log_error("fx", "autopan still on after reaching the middle (%.3f %.3f)",
                      left, right);
            goto done;
        }
    }

    /* Turned off while moving out, a tenth of the way through the swing
     * (about 59% left), it levels off well short of the side, comes back,
     * and stops at the middle.
     */
    fx_setup(st, &p);
    st->ap_rng = 0;
    fx_set_on(st, 0, 0, 1, 0, 0, 0, 0, 0);
    for (i = 0; i < 2205; i++) {
        left = 0.5f;
        right = 0.5f;
        fx_process(st, &left, &right);
    }
    fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
    {
        float low_r = 1;

        for (i = 0; i < 8000; i++) {
            left = 0.5f;
            right = 0.5f;
            fx_process(st, &left, &right);
            low_r = right < low_r ? right : low_r;
            if (i >= 7000 && (left != 0.5f || right != 0.5f)) {
                log_error("fx", "autopan did not return to the middle (%.3f %.3f)", left, right);
                goto done;
            }
        }
        if (low_r < 0.12f) {
            log_error("fx", "autopan kept going out after being turned off (right %.3f)", low_r);
            goto done;
        }
    }

    /* Started again and again, it heads both ways. */
    {
        int went_left = 0;
        int went_right = 0;
        int run;

        st->ap_rng = 12345;
        for (run = 0; run < 16; run++) {
            fx_set_on(st, 0, 0, 1, 0, 0, 0, 0, 0);
            for (i = 0; i < 2000; i++) {
                left = 0.5f;
                right = 0.5f;
                fx_process(st, &left, &right);
            }
            went_left += left > right;
            went_right += right > left;
            fx_set_on(st, 0, 0, 0, 0, 0, 0, 0, 0);
            for (i = 0; i < 30000; i++) {
                left = 0.5f;
                right = 0.5f;
                fx_process(st, &left, &right);
            }
        }
        if (!went_left || !went_right || went_left + went_right != 16) {
            log_error("fx", "autopan start direction not random (%d left, %d right)",
                      went_left, went_right);
            goto done;
        }
    }
    fx_set_beat(st, 0);

    /* The boost is there from the first sample. */
    fx_defaults(&p);
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 1, 0, 0, 0, 0);
    left = 0.5f;
    right = 0.5f;
    fx_process(st, &left, &right);
    if (left < 0.57f || right < 0.57f) {
        log_error("fx", "phaser boost not immediate (%.3f %.3f)", left, right);
        goto done;
    }

    /* The phaser notches sweep across a 1 kHz tone: deep dips, no big peaks. */
    fx_defaults(&p);
    p.phaser_on = 1;
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 1, 0, 0, 0, 0);
    fx_set_beat(st, 0.5f);
    {
        float lo = 9;
        float hi = 0;
        int blk;

        for (blk = 0; blk < 200; blk++) {
            double sum = 0;

            for (i = 0; i < 441; i++) {
                left = 0.5f * sinf(2.0f * PV_PI * 1000.0f * (float)(blk * 441 + i) / 44100.0f);
                right = left;
                fx_process(st, &left, &right);
                sum += (double)left * left;
            }
            if (blk > 50) {
                float rms = (float)sqrt(sum / 441) / 0.35355f;

                lo = rms < lo ? rms : lo;
                hi = rms > hi ? rms : hi;
            }
        }
        if (lo > 0.25f || hi > 1.3f || hi < 0.8f) {
            log_error("fx", "phaser 1 kHz gain from %.3f to %.3f", lo, hi);
            goto done;
        }
    }
    fx_set_beat(st, 0);

    /* Chorus and flanger off, and with mix 0, leave the song alone. */
    fx_defaults(&p);
    p.chorus_on = 1;
    p.ch_mix = 0;
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 1, 0, 0, 0);
    for (i = 0; i < 200; i++) {
        left = dry[i];
        right = dry[i];
        fx_process(st, &left, &right);
        wet[i] = left;
    }
    if (!near_same(dry, wet, 200, 0.00001f)) {
        log_error("fx", "chorus with mix 0 was not dry");
        goto done;
    }
    fx_defaults(&p);
    p.flanger_on = 1;
    p.fl_mix = 0;
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
    for (i = 0; i < 200; i++) {
        left = dry[i];
        right = dry[i];
        fx_process(st, &left, &right);
        wet[i] = left;
    }
    if (!near_same(dry, wet, 200, 0.00001f)) {
        log_error("fx", "flanger with mix 0 was not dry");
        goto done;
    }

    /* One chorus voice is one late copy. Two voices are two copies, a
     * little apart. The right side drifts ahead when depth is up.
     */
    {
        enum { M = 2000 };
        float *cl = calloc(M, sizeof(float));
        float *cr = calloc(M, sizeof(float));
        int lag_l;
        int lag_r;
        float amp;

        if (!cl || !cr) {
            free(cl);
            free(cr);
            log_error("fx", "out of memory");
            goto done;
        }
        fx_defaults(&p);
        p.chorus_on = 1;
        p.ch_delay = 20;
        p.ch_depth = 0;
        p.ch_voices = 1;
        p.ch_mix = 100;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 1, 0, 0, 0);
        left = 0;
        right = 0;
        fx_process(st, &left, &right);
        st->ch_phase = 0;
        st->ch_mix = 1;
        memset(st->ch_buf[0], 0, (size_t)st->ch_len * 2 * sizeof(float));
        st->ch_w = 0;
        for (i = 0; i < M; i++) {
            left = i == 0 ? 1.0f : 0;
            right = left;
            fx_process(st, &left, &right);
            cl[i] = left;
            cr[i] = right;
        }
        lag_l = 0;
        amp = 0;
        for (i = 1; i < M; i++) {
            if (fabsf(cl[i]) > amp) {
                amp = fabsf(cl[i]);
                lag_l = i;
            }
        }
        if (lag_l != 882 || amp < 0.9f) {
            log_error("fx", "chorus copy landed at %d (%.3f), wanted sample 882", lag_l, amp);
            free(cl);
            free(cr);
            goto done;
        }

        p.ch_voices = 2;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 1, 0, 0, 0);
        left = 0;
        right = 0;
        fx_process(st, &left, &right);
        st->ch_phase = 0;
        st->ch_mix = 1;
        memset(st->ch_buf[0], 0, (size_t)st->ch_len * 2 * sizeof(float));
        st->ch_w = 0;
        for (i = 0; i < M; i++) {
            left = i == 0 ? 1.0f : 0;
            right = left;
            fx_process(st, &left, &right);
            cl[i] = left;
        }
        /* 20 ms ± 1.2 ms at 44100 Hz: peaks near 829 and 935. */
        if (cl[829] < 0.3f || cl[935] < 0.3f || fabsf(cl[880]) > 0.05f) {
            log_error("fx", "chorus voices were not separate (%.3f %.3f %.3f)",
                      cl[829], cl[880], cl[935]);
            free(cl);
            free(cr);
            goto done;
        }

        p.ch_voices = 1;
        p.ch_depth = 8;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 1, 0, 0, 0);
        left = 0;
        right = 0;
        fx_process(st, &left, &right);
        st->ch_phase = 0;
        st->ch_mix = 1;
        memset(st->ch_buf[0], 0, (size_t)st->ch_len * 2 * sizeof(float));
        st->ch_w = 0;
        for (i = 0; i < M; i++) {
            left = i == 0 ? 1.0f : 0;
            right = left;
            /* Hold the oscillator at the start of its cycle. Otherwise it
             * moves while the copy is in the delay and the landing shifts.
             */
            st->ch_phase = 0;
            fx_process(st, &left, &right);
            cl[i] = left;
            cr[i] = right;
        }
        lag_l = 0;
        lag_r = 0;
        amp = 0;
        for (i = 1; i < M; i++) {
            if (fabsf(cl[i]) > amp) {
                amp = fabsf(cl[i]);
                lag_l = i;
            }
        }
        amp = 0;
        for (i = 1; i < M; i++) {
            if (fabsf(cr[i]) > amp) {
                amp = fabsf(cr[i]);
                lag_r = i;
            }
        }
        if (lag_l != 882 || lag_r != 1235) {
            log_error("fx", "chorus pitch drift lags %d and %d", lag_l, lag_r);
            free(cl);
            free(cr);
            goto done;
        }
        free(cl);
        free(cr);
    }

    /* Flanger delay is about a millisecond, not the chorus's 20 ms.
     * Half a millisecond against a 1 kHz tone cancels; a full millisecond
     * adds. Sweeping between them is the whoosh.
     */
    {
        fx_defaults(&p);
        p.flanger_on = 1;
        p.fl_delay = 1;
        p.fl_depth = 0;
        p.fl_feedback = 0;
        p.fl_mix = 100;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
        left = 0;
        right = 0;
        fx_process(st, &left, &right);
        st->fl_phase = 0;
        st->fl_mix = 1;
        memset(st->fl_buf[0], 0, (size_t)st->fl_len * 2 * sizeof(float));
        st->fl_w = 0;
        {
            int lag = 0;
            float amp = 0;

            for (i = 0; i < 200; i++) {
                left = i == 0 ? 1.0f : 0;
                right = left;
                fx_process(st, &left, &right);
                if (fabsf(left) > amp) {
                    amp = fabsf(left);
                    lag = i;
                }
            }
            if (lag != 44 || amp < 0.8f) {
                log_error("fx", "flanger copy landed at %d (%.3f), wanted sample 44", lag, amp);
                goto done;
            }
        }

        fx_defaults(&p);
        p.flanger_on = 1;
        p.fl_delay = 0.5f;
        p.fl_depth = 0;
        p.fl_feedback = 0;
        p.fl_mix = 50;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
        {
            double sum = 0;

            for (i = 0; i < 4000; i++) {
                left = 0.5f * sinf(2.0f * PV_PI * 1000.0f * (float)i / 44100.0f);
                right = left;
                fx_process(st, &left, &right);
                if (i >= 2000) {
                    sum += (double)left * left;
                }
            }
            sum = sqrt(sum / 2000) / 0.35355;
            if (sum > 0.2) {
                log_error("fx", "flanger half-millisecond delay did not cancel (gain %.3f)", sum);
                goto done;
            }
        }

        p.fl_delay = 1;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
        {
            double plain = 0;
            double fed = 0;

            for (i = 0; i < 8000; i++) {
                left = 0.25f * sinf(2.0f * PV_PI * 1000.0f * (float)i / 44100.0f);
                right = left;
                fx_process(st, &left, &right);
                if (i >= 4000) {
                    plain += (double)left * left;
                }
                if (left != left) {
                    log_error("fx", "flanger produced a non-number");
                    goto done;
                }
            }
            p.fl_feedback = 0.6f;
            fx_setup(st, &p);
            fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
            for (i = 0; i < 8000; i++) {
                left = 0.25f * sinf(2.0f * PV_PI * 1000.0f * (float)i / 44100.0f);
                right = left;
                fx_process(st, &left, &right);
                if (i >= 4000) {
                    fed += (double)left * left;
                }
                if (left != left || fabsf(left) > 2.0f) {
                    log_error("fx", "flanger feedback ran away (%.3f)", left);
                    goto done;
                }
            }
            if (fed < plain * 1.3) {
                log_error("fx", "flanger feedback did not strengthen the comb (%.4f vs %.4f)",
                          fed, plain);
                goto done;
            }
        }

        fx_defaults(&p);
        p.flanger_on = 1;
        p.fl_period = 0.25f;
        p.fl_delay = 1;
        p.fl_depth = 0.7f;
        p.fl_feedback = 0;
        p.fl_mix = 50;
        fx_setup(st, &p);
        fx_set_on(st, 0, 0, 0, 0, 0, 1, 0, 0);
        fx_set_beat(st, 0.5f);
        {
            float lo = 9;
            float hi = 0;
            int blk;

            for (blk = 0; blk < 80; blk++) {
                double sum = 0;

                for (i = 0; i < 441; i++) {
                    int n = blk * 441 + i;

                    left = 0.5f * sinf(2.0f * PV_PI * 1000.0f * (float)n / 44100.0f);
                    right = left;
                    fx_process(st, &left, &right);
                    sum += (double)left * left;
                    if (left != left || fabsf(left) > 2.0f) {
                        log_error("fx", "flanger sweep produced %.3f", left);
                        goto done;
                    }
                }
                if (blk > 8) {
                    float rms = (float)sqrt(sum / 441) / 0.35355f;

                    lo = rms < lo ? rms : lo;
                    hi = rms > hi ? rms : hi;
                }
            }
            if (lo > 0.45f || hi < 0.75f) {
                log_error("fx", "flanger sweep gain from %.3f to %.3f", lo, hi);
                goto done;
            }
        }
        fx_set_beat(st, 0);
    }

    if (underwater_self_test() != 0 || lasers_self_test() != 0) {
        goto done;
    }
    rc = 0;
done:
    free(dry);
    free(wet);
    fx_free(st);
    return rc;
}
