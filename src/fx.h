#ifndef AUTODJ_FX_H
#define AUTODJ_FX_H

/* Reverb, chipmunk (a pitch shifter), autopan, phaser, chorus, flanger,
 * underwater, and lasers.
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
    float rv_send;  /* 0..1, default 0.5. How much reverb is added now. */
    float rv_send_min; /* 0..1, default 0. Send with the reverb knob all the way down. */
    float rv_send_max; /* 0..1, default 1. Send with the reverb knob all the way up. */
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
    int chorus_on;
    float ch_period; /* 0.25..16, default 1. Measures for one pitch drift
                      * up and down. 120 BPM when the tempo is unknown. */
    float ch_delay;  /* 5..40 ms, default 20. How late the copies are. */
    float ch_depth;  /* 0..10 ms, default 5, and below ch_delay. How far
                      * the delay swings, which is the pitch drift. */
    int ch_voices;   /* 1..4, default 2. Separate copies of the song. */
    float ch_mix;    /* 0..100, default 50. Percent of the copies in the
                      * mix. 0 leaves the song unchanged. */
    int flanger_on;
    float fl_period;   /* 0.25..16, default 2. Measures for one sweep. */
    float fl_delay;    /* 0.2..5 ms, default 1. Much shorter than chorus. */
    float fl_depth;    /* 0..4 ms, default 0.5, and below fl_delay. How far
                        * the short delay sweeps. */
    float fl_feedback; /* 0..0.9, default 0.5. Feeds the delay back on
                        * itself. Higher is the jet-plane ring. */
    float fl_mix;      /* 0..100, default 50. Percent of delayed sound.
                        * 50 gives the deepest notches. 0 is dry. */
    int underwater_on;
    float uw_cutoff;   /* 100..4000 Hz, default 500. Steep low-pass: lower
                        * sounds deeper and more muffled. */
    float uw_res;      /* 0..1, default 0.4. A hollow, boomy bump at the
                        * cutoff. */
    float uw_period;   /* 0.25..16, default 0.5. Measures for one slow
                        * wobble of the pitch. 120 BPM when unknown. */
    float uw_wobble;   /* 0..5 ms, default 1.5. Delay swing behind the
                        * wobble. 0 holds the pitch steady. */
    float uw_mix;      /* 0..100, default 100. Percent underwater sound. */
    float uw_boost;    /* 0..100, default 20. Percent louder, to make up
                        * for the highs the filter takes out. */
    int lasers_on;
    float lz_rate;   /* 0.2..20, default 4. Average zaps per second. */
    float lz_low;    /* 80..4000 Hz, default 280. Lowest zap. */
    float lz_high;   /* 80..6000 Hz, default 700, and at least lz_low.
                      * Highest zap. */
    float lz_length; /* 30..400 ms, default 260. How long one zap lasts. */
    float lz_level;  /* 0..100, default 30. How loud the zaps are. The
                      * song stays; 0 adds nothing. The laser knob moves
                      * this while a song plays. */
} FxParams;

typedef struct FxState FxState;

void fx_defaults(FxParams *p);

/* rate is the output sample rate. Returns NULL on allocation failure. */
FxState *fx_new(unsigned rate);
void fx_free(FxState *st);
void fx_setup(FxState *st, const FxParams *p);
/* The power buttons. A change takes effect on the next sample. */
/* How much reverb is added, 0..1. The change is smoothed, so it can move
 * while a song plays. */
void fx_set_send(FxState *st, float send);
/* Laser volume, 0..100. Heard on the next sample. */
void fx_set_lasers(FxState *st, float level);
void fx_set_on(FxState *st, int reverb, int pitch, int autopan, int phaser,
               int chorus, int flanger, int underwater, int lasers);
/* Length of one beat in seconds at the current playback speed, or 0 when
 * the tempo is unknown. The autopan period counts beats when this is set.
 */
void fx_set_beat(FxState *st, float beat_seconds);
void fx_process(FxState *st, float *left, float *right);

int fx_self_test(void);

#endif
