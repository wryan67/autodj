#ifndef AUTODJ_PLAYER_H
#define AUTODJ_PLAYER_H

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
void player_volume_step(int direction);

int player_self_test(void);

#endif
