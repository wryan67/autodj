#ifndef AUTODJ_PLAYLIST_H
#define AUTODJ_PLAYLIST_H

#include <stddef.h>

typedef struct Playlist Playlist;

Playlist *playlist_load(const char *folder, int shuffle);
void playlist_free(Playlist *pl);

/* Play every song whose file name starts with prefix, ignoring letter case,
 * before the normal order, in alphabetical order. A leading track number,
 * such as "001-" in "001-smooth-criminal.mp3", may be left out of prefix.
 * A folder in front of prefix is ignored. A song still waiting in the queue
 * is not queued again. Queued songs are left out of the rest of the first
 * pass through the folder. Returns how many songs matched, including ones
 * already waiting. */
int playlist_queue_prefix(Playlist *pl, const char *prefix);

/* Copy the next path into dest: a queued song first, then the normal order.
 * Loops forever. Returns 0 on success. */
int playlist_next(Playlist *pl, char *dest, size_t destlen);
size_t playlist_count(const Playlist *pl);

int playlist_self_test(void);

#endif
