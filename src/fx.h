#ifndef AUTODJ_FX_H
#define AUTODJ_FX_H

/* Reverb, chipmunk (a pitch shifter), autopan, and phaser.
 *
 * The knob names and the default numbers are the ones Mixxx shows for its
 * built-in Reverb, Pitch Shift, and Autopan. The processing here is separate.
 * Mixxx is GPL-2.0-or-later and its pitch shifter is Rubber Band, which is
 * also GPL, so none of that source is used. Like Rubber Band, the pitch
 * shifter here works on the spectrum (a phase vocoder), not on raw delay
 * reads, so it does not warble the whole mix.
 */

/* The *_on fields only set the starting state for fx_setup. The config
 * never sets them; autodj always starts with every effect off. */
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
    int autopan_on;
    float ap_period; /* 0.25..16, default 4. Measures (4 beats) for one swing
                      * left, right, and back, so 4 is back at the middle
                      * every other measure. 120 BPM when the tempo is
                      * unknown. */
    float ap_smooth; /* 0.25..0.5, default 0.5. 0.5 is a smooth swing, lower
                      * waits longer at each side. */
    float ap_distance; /* 0..100, default 75. Percent from the middle toward
                        * each speaker. 100 is all the way. */
    int phaser_on;
    float ph_period;   /* 0.25..16, default 1. Measures for one sweep up and
                        * down. 120 BPM when the tempo is unknown. */
    float ph_low;      /* 20..20000 Hz, default 300. Bottom of the sweep. */
    float ph_high;     /* 20..20000 Hz, default 3000. Top, above ph_low. */
    int ph_stages;     /* 1..12, default 6. Allpasses; two per notch. */
    float ph_feedback; /* 0..0.9, default 0.5. Sharper, ringing notches. */
    float ph_blend;    /* 0..100, default 50. Percent phased sound in the
                        * mix. 50 gives the deepest notches. */
    float ph_boost;    /* 0..100, default 15. Percent louder while on, to
                        * make up for what the notches take out. */
} FxParams;

typedef struct FxState FxState;

void fx_defaults(FxParams *p);

/* rate is the output sample rate. Returns NULL on allocation failure. */
FxState *fx_new(unsigned rate);
void fx_free(FxState *st);
void fx_setup(FxState *st, const FxParams *p);
/* The power buttons. A change takes effect on the next sample. */
void fx_set_on(FxState *st, int reverb, int pitch, int autopan, int phaser);
/* Length of one beat in seconds at the current playback speed, or 0 when
 * the tempo is unknown. The autopan period counts beats when this is set.
 */
void fx_set_beat(FxState *st, float beat_seconds);
void fx_process(FxState *st, float *left, float *right);

int fx_self_test(void);

#endif
