#ifndef AUTODJ_PLAYLIST_H
#define AUTODJ_PLAYLIST_H

#include <stddef.h>

typedef struct Playlist Playlist;

Playlist *playlist_load(const char *folder, int shuffle);
void playlist_free(Playlist *pl);

/* Copy the next path into dest. Loops forever. Returns 0 on success. */
int playlist_next(Playlist *pl, char *dest, size_t destlen);
size_t playlist_count(const Playlist *pl);

#endif
