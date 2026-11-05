#ifndef AUTODJ_PLAYER_H
#define AUTODJ_PLAYER_H

#include "playlist.h"

typedef struct PlayerSettings {
    int overlap_sec;
    int pause_sec;
    int resume_sec;
    int trim;
    unsigned rate;
} PlayerSettings;

int player_start(const PlayerSettings *settings, Playlist *playlist);
void player_stop(void);
int player_alive(void);

void player_toggle_pause(void);
void player_skip(void);
void player_set_volume_percent(int percent);
int player_volume_percent(void);
void player_volume_step(int direction);

int player_self_test(void);

#endif
