#ifndef AUTODJ_PLAYLIST_H
#define AUTODJ_PLAYLIST_H

#include <stddef.h>

typedef struct Playlist Playlist;

/* Songs in folder, in alphabetical order, shuffled when shuffle is set.
 * The queue lives only in memory. */
Playlist *playlist_load(const char *folder, int shuffle);

/* Same folder scan, with the play order stored in cache_path. The file is
 * one song file name per line. The first line is the next song. A song that
 * starts playing moves to the end. Names already in the file keep their
 * order. Songs in the folder that are missing from the file are shuffled
 * when shuffle is set, or appended in alphabetical order when it is not,
 * and then written to the end. A name that is no longer in the folder is
 * dropped. cache_path NULL is the same as playlist_load. */
Playlist *playlist_open(const char *folder, int shuffle, const char *cache_path);

void playlist_free(Playlist *pl);

/* Play every song whose file name starts with prefix, ignoring letter case,
 * before the normal order, in alphabetical order. A leading track number,
 * such as "001-" in "001-smooth-criminal.mp3", may be left out of prefix.
 * A folder in front of prefix is ignored. A song still waiting in the queue
 * is not queued again. Without a cache file, queued songs are left out of
 * the rest of the first pass through the folder. With a cache file, after
 * the queued songs have played, those songs are moved to the bottom of the
 * saved list (their order kept) and the next song is the first that was
 * not requested. Returns how many songs matched, including ones already
 * waiting. */
int playlist_queue_prefix(Playlist *pl, const char *prefix);

/* Copy the next path into dest and remove it from the front of the list.
 * A queued song comes first. Loops forever. Returns 0 on success. */
int playlist_next(Playlist *pl, char *dest, size_t destlen);

/* Copy the next path without removing it. Returns 0 on success. */
int playlist_peek(Playlist *pl, char *dest, size_t destlen);

/* path has started. When it is the song at the front, that song moves to
 * the end of a cache-backed list. */
void playlist_started(Playlist *pl, const char *path);

/* Write the cache file if the order changed. */
void playlist_flush(Playlist *pl);

size_t playlist_count(const Playlist *pl);

int playlist_self_test(void);

#endif
