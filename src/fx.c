#include "fx.h"

#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Reverb is a plate reverb: input smoothing, four diffusing allpasses, and
 * two cross-fed tanks with slowly moving allpasses, read at several points
 * for a wide stereo tail. Pitch change is a phase vocoder, so a full mix
 * keeps its steady tones and only the pitch moves. Autopan moves the
 * balance from one side to the other and back, in time with the beat.
 * The phaser is a chain of allpass filters whose notches sweep up and down
 * once per measure.
 */

#define AP_DELAY 512
#define PH_STAGES 6
#define PV_OSAMP 8
#define PV_PI 3.14159265358979323846f

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
    int ph_on;
    int ph_live;
    float beat_sec;
    int ap_on;
    int ap_live;
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
    p->ps_pitch = 1.0f;
    p->ps_range = 1.0f;
    p->ps_semi = 1;
    p->ps_formant = 0;
    p->ap_period = 4.0f;
    p->ap_smooth = 0.5f;
    p->ap_distance = 75.0f;
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
}

static void ph_clear(FxState *st)
{
    memset(st->ph_x, 0, sizeof st->ph_x);
    memset(st->ph_y, 0, sizeof st->ph_y);
    memset(st->ph_fb, 0, sizeof st->ph_fb);
    st->ph_phase = 0;
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
    st->rv_send = 0;
    st->rv_live = 0;
    st->rv_lfo[0] = 0;
    st->rv_lfo[1] = 1.7f;
    st->ps_mix = 0;
    st->ap_mix = 0;
    st->ap_live = 0;
    st->ph_mix = 0;
    st->ph_live = 0;
    st->rv_on = st->p.reverb_on ? 1 : 0;
    st->ps_on = st->p.pitch_on ? 1 : 0;
    st->ap_on = st->p.autopan_on ? 1 : 0;
    st->ph_on = st->p.phaser_on ? 1 : 0;
}

void fx_set_on(FxState *st, int reverb, int pitch, int autopan, int phaser)
{
    if (!st) {
        return;
    }
    st->rv_on = reverb ? 1 : 0;
    st->ps_on = pitch ? 1 : 0;
    st->ap_on = autopan ? 1 : 0;
    st->ph_on = phaser ? 1 : 0;
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

/* Six first-order allpasses with feedback, mixed half and half with the
 * input. Where the chain turns the phase around, the two halves cancel, so
 * three notches sweep between 300 Hz and 3 kHz, once up and down per
 * measure.
 */
static void process_phaser(FxState *st, float *left, float *right)
{
    const float fb = 0.5f;
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

    sweep = 4.0 * (st->beat_sec > 0 ? st->beat_sec : 0.5);
    st->ph_phase += 1.0 / (sweep * (double)st->rate);
    if (st->ph_phase >= 1.0) {
        st->ph_phase -= floor(st->ph_phase);
    }
    freq = 300.0f * powf(10.0f, 0.5f - 0.5f * cosf(2.0f * PV_PI * (float)st->ph_phase));
    if (freq > 0.45f * (float)st->rate) {
        freq = 0.45f * (float)st->rate;
    }
    t = tanf(PV_PI * freq / (float)st->rate);
    a = (t - 1.0f) / (t + 1.0f);

    io[0] = left;
    io[1] = right;
    for (c = 0; c < 2; c++) {
        float x = *io[c];
        float v = x + fb * st->ph_fb[c];

        for (k = 0; k < PH_STAGES; k++) {
            float y = a * v + st->ph_x[c][k] - a * st->ph_y[c][k];

            st->ph_x[c][k] = v;
            st->ph_y[c][k] = y;
            v = y;
        }
        st->ph_fb[c] = v;
        /* 0.75 keeps the feedback peaks near the input level. */
        v = 0.75f * 0.5f * (x + v);
        *io[c] = fin(x + st->ph_mix * (v - x));
    }
}

static void process_autopan(FxState *st, float *left, float *right)
{
    double period;
    float width;
    float smooth;
    float hold;
    float boost;
    float pos;
    float frac;
    float gl;
    float gr;
    float shrink;
    float delay;
    float l;
    float r;
    double half;

    if (!st->ap_live) {
        if (!st->ap_on) {
            return;
        }
        /* Starts at the middle of the swing. */
        ap_clear(st);
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
    half = st->ap_phase < 0.5 ? 0.5 : 1.0;
    st->ap_phase += 1.0 / (period * (double)st->rate);
    if (!st->ap_on && st->ap_phase >= half) {
        /* Turned off: the swing ran on to the middle, where the song is
         * back as it was, so stop here.
         */
        ap_clear(st);
        st->ap_mix = 0;
        st->ap_live = 0;
        return;
    }
    if (st->ap_phase >= 1.0) {
        st->ap_phase -= floor(st->ap_phase);
    }

    width = clamp_unit(st->p.ap_distance * 0.01f) * st->ap_mix;
    smooth = st->p.ap_smooth;
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
    pos = boost * sinf(2.0f * PV_PI * (float)st->ap_phase);
    if (pos > 1.0f) {
        pos = 1.0f;
    }
    if (pos < -1.0f) {
        pos = -1.0f;
    }
    pos *= width;

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

void fx_process(FxState *st, float *left, float *right)
{
    if (!st || !left || !right) {
        return;
    }
    process_reverb(st, left, right);
    process_pitch(st, left, right);
    process_phaser(st, left, right);
    process_autopan(st, left, right);
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
    fx_set_on(st, 0, 0, 0, 0);
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
    fx_set_on(st, 1, 0, 0, 0);
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
    fx_set_on(st, 1, 0, 0, 0);
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
    fx_set_on(st, 0, 1, 0, 0);
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
    fx_set_on(st, 0, 1, 0, 0);
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
    fx_set_on(st, 0, 0, 1, 0);
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
    fx_set_on(st, 0, 0, 1, 0);
    for (i = 0; i < 5512; i++) {
        left = 0.5f;
        right = 0.5f;
        fx_process(st, &left, &right);
    }
    fx_set_on(st, 0, 0, 0, 0);
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
    fx_set_beat(st, 0);

    /* The phaser notches sweep across a 1 kHz tone: deep dips, no big peaks. */
    fx_defaults(&p);
    p.phaser_on = 1;
    fx_setup(st, &p);
    fx_set_on(st, 0, 0, 0, 1);
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

    rc = 0;
done:
    free(dry);
    free(wet);
    fx_free(st);
    return rc;
}
