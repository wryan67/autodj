#ifndef AUTODJ_FX_H
#define AUTODJ_FX_H

/* Reverb, pitch shift, and phaser.
 *
 * The knob names and the default numbers are the ones Mixxx shows for its
 * built-in Reverb, Pitch Shift, and Phaser. The processing here is separate.
 * Mixxx is GPL-2.0-or-later and its pitch shifter is Rubber Band, which is
 * also GPL, so none of that source is used. Like Rubber Band, the pitch
 * shifter here works on the spectrum (a phase vocoder), not on raw delay
 * reads, so it does not warble the whole mix.
 */

typedef struct FxParams {
    int reverb_on;
    float rv_decay; /* 0..1, default 0.5. Higher lasts longer. */
    float rv_bw;    /* 0..1, default 1. Higher keeps more highs going in. */
    float rv_damp;  /* 0..1, default 0. Higher dulls the tail. */
    float rv_send;  /* 0..1, default 0.5. How much reverb is added. */
    int pitch_on;
    float ps_pitch; /* -1..1, default 1. One octave up. 0 leaves the pitch alone. */
    float ps_range; /* 0..2, default 1. Octaves from the middle to either end. */
    int ps_semi;    /* default on. Step by semitones. */
    int ps_formant; /* default off. Keeps the voice's tone color (less chipmunk). */
    int phaser_on;
    float ph_period;   /* 0.25..4 seconds, default 1. */
    float ph_feedback; /* -1..1, default 0. */
    float ph_range;    /* 0.05..1, default 1. How wide the notches sweep. */
    int ph_stages;     /* 1..12, default 7. */
    float ph_depth;    /* 0.5..1, default 0.5. */
    int ph_triplets;   /* default off. Period divided by 3. */
    int ph_stereo;     /* default off. Right channel sweeps out of phase. */
} FxParams;

typedef struct FxState FxState;

void fx_defaults(FxParams *p);

/* rate is the output sample rate. Returns NULL on allocation failure. */
FxState *fx_new(unsigned rate);
void fx_free(FxState *st);
void fx_setup(FxState *st, const FxParams *p);
/* The power buttons. A change takes effect on the next sample. */
void fx_set_on(FxState *st, int reverb, int pitch, int phaser);
void fx_process(FxState *st, float *left, float *right);

int fx_self_test(void);

#endif
