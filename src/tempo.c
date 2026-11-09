#include "tempo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Tempo is found from an onset curve: the rise in loudness of three bands
 * (bass, middle, top) every 5 ms. The curve's autocorrelation peaks at the
 * beat spacing. Each candidate is scored with its double and half spacing
 * as well, then weighted toward 120 BPM so an octave mistake is less likely.
 */

#define ENV_RATE 200
#define BANDS 3
#define MIN_BPM 60.0
#define MAX_BPM 200.0
#define MAX_SECONDS 120

static double ac_at(const double *ac, int n, double lag)
{
    int i = (int)lag;
    double f = lag - (double)i;

    if (i < 0 || i + 1 >= n) {
        return 0;
    }
    return ac[i] * (1.0 - f) + ac[i + 1] * f;
}

double tempo_detect(const float *pcm, size_t frames, unsigned rate)
{
    size_t hop;
    size_t first;
    size_t count;
    size_t nenv;
    size_t i;
    double *env = NULL;
    double *ac = NULL;
    double lp1 = 0;
    double lp2 = 0;
    double prev[BANDS] = {0, 0, 0};
    double a1;
    double a2;
    double mean;
    double best_score = 0;
    double best_bpm = 0;
    double bpm;
    int max_lag;
    int min_lag;
    int lag;

    if (!pcm || rate < 8000) {
        return 0;
    }
    hop = rate / ENV_RATE;
    count = frames;
    if (count > (size_t)rate * MAX_SECONDS) {
        count = (size_t)rate * MAX_SECONDS;
    }
    first = (frames - count) / 2;
    nenv = count / hop;
    if (nenv < (size_t)ENV_RATE * 10) {
        return 0;
    }
    env = calloc(nenv, sizeof *env);
    max_lag = (int)(60.0 * ENV_RATE / MIN_BPM * 4.0) + 2;
    ac = calloc((size_t)max_lag + 1, sizeof *ac);
    if (!env || !ac) {
        free(env);
        free(ac);
        return 0;
    }

    a1 = 1.0 - exp(-2.0 * M_PI * 150.0 / (double)rate);
    a2 = 1.0 - exp(-2.0 * M_PI * 2500.0 / (double)rate);
    for (i = 0; i < nenv; i++) {
        double e[BANDS] = {0, 0, 0};
        double onset = 0;
        size_t j;
        int b;

        for (j = 0; j < hop; j++) {
            const float *s = pcm + 2 * (first + i * hop + j);
            double x = 0.5 * ((double)s[0] + (double)s[1]);
            double lo;
            double mid;
            double hi;

            lp1 += a1 * (x - lp1);
            lp2 += a2 * (x - lp2);
            lo = lp1;
            mid = lp2 - lp1;
            hi = x - lp2;
            e[0] += lo * lo;
            e[1] += mid * mid;
            e[2] += hi * hi;
        }
        for (b = 0; b < BANDS; b++) {
            double v = log(1.0 + 1000.0 * e[b] / (double)hop);
            double d = v - prev[b];

            prev[b] = v;
            if (d > 0 && i > 0) {
                onset += d;
            }
        }
        env[i] = onset;
    }

    /* Take away the slow level so only the beats are left. */
    {
        int half = ENV_RATE / 4;
        double *tmp = malloc(nenv * sizeof *tmp);
        double sum = 0;
        size_t lo = 0;
        size_t hi = 0;

        if (!tmp) {
            free(env);
            free(ac);
            return 0;
        }
        for (i = 0; i < nenv; i++) {
            size_t want_lo = i > (size_t)half ? i - (size_t)half : 0;
            size_t want_hi = i + (size_t)half + 1 < nenv ? i + (size_t)half + 1 : nenv;

            while (hi < want_hi) {
                sum += env[hi++];
            }
            while (lo < want_lo) {
                sum -= env[lo++];
            }
            tmp[i] = env[i] - sum / (double)(hi - lo);
            if (tmp[i] < 0) {
                tmp[i] = 0;
            }
        }
        memcpy(env, tmp, nenv * sizeof *env);
        free(tmp);
    }
    mean = 0;
    for (i = 0; i < nenv; i++) {
        mean += env[i];
    }
    mean /= (double)nenv;
    for (i = 0; i < nenv; i++) {
        env[i] -= mean;
    }

    min_lag = (int)(60.0 * ENV_RATE / MAX_BPM / 2.0);
    for (lag = min_lag; lag <= max_lag; lag++) {
        double s = 0;

        if ((size_t)lag >= nenv) {
            break;
        }
        for (i = 0; i + (size_t)lag < nenv; i++) {
            s += env[i] * env[i + (size_t)lag];
        }
        ac[lag] = s / (double)(nenv - (size_t)lag);
    }

    for (bpm = MIN_BPM; bpm <= MAX_BPM; bpm += 0.05) {
        double l = 60.0 * ENV_RATE / bpm;
        double s = ac_at(ac, max_lag + 1, l) +
                   0.5 * ac_at(ac, max_lag + 1, 2.0 * l) +
                   0.25 * ac_at(ac, max_lag + 1, 4.0 * l) +
                   0.5 * ac_at(ac, max_lag + 1, 0.5 * l);
        double oct = log2(bpm / 120.0);
        double w = exp(-0.5 * (oct / 0.6) * (oct / 0.6));

        s *= w;
        if (s > best_score) {
            best_score = s;
            best_bpm = bpm;
        }
    }
    free(env);
    free(ac);
    return best_score > 0 ? best_bpm : 0;
}
