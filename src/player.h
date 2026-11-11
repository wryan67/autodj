#ifndef AUTODJ_PLAYER_H
#define AUTODJ_PLAYER_H

#include "fx.h"
#include "playlist.h"

typedef struct PlayerSettings {
    int overlap_sec;
    int pause_sec;
    int resume_sec;
    int trim;
    int loudness;
    int loudness_lufs;
    unsigned rate;
} PlayerSettings;

typedef struct JogSettings {
    double turn_sec;   /* audio moved by one turn */
    int clicks;        /* clicks in one turn */
    double max_speed;  /* fastest scratch, times normal speed */
    double spinup_sec; /* back to normal speed after the dial stops */
    double grab_sec;   /* normal playback held back when the dial starts */
    double smooth_sec; /* how quickly the speed follows the dial */
} JogSettings;

int player_start(const PlayerSettings *settings, Playlist *playlist);
void player_stop(void);
int player_alive(void);

void player_toggle_pause(void);
void player_skip(void);
void player_previous(void);
void player_rewind(int ms);
/* One click of the jog dial. direction is 1 or -1; when_us is the event
 * time in microseconds, used to measure how fast the dial is turning. */
void player_jog(int direction, long long when_us);
/* Change the dial behavior. Takes effect within one audio period, so it
 * can be changed while a song plays. */
void player_set_jog(const JogSettings *jog);
/* Current playback speed (negative while turning back) and whether the
 * dial is holding the song. */
double player_speed(int *jogging);
/* Name of the song now playing, or "" before the first one. */
void player_now_playing(char *dest, size_t n);
void player_set_volume_percent(int percent);
int player_volume_percent(void);
/* direction is +1 or -1. percent is the size of one step. The shortcut
 * remote uses 1. Keyboard volume uses 5. */
void player_volume_step(int direction, int percent);

enum {
    PLAYER_FX_REVERB = 0,
    PLAYER_FX_PITCH = 1,
    PLAYER_FX_AUTOPAN = 2,
    PLAYER_FX_PHASER = 3,
    PLAYER_FX_CHORUS = 4,
    PLAYER_FX_FLANGER = 5
};

/* Copy the effect settings. Call this before player_start. The power
 * buttons can change while a song plays. */
void player_set_fx(const FxParams *fx);
int player_fx_enabled(int which);
/* Returns the new state, 1 on and 0 off. */
int player_fx_toggle(int which);
/* Turn every effect off. A second press leaves them off. */
void player_fx_all_off(void);

int player_self_test(void);

#endif
