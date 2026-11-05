#include "playlist.h"

#include "log.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

struct Playlist {
    char **paths;
    size_t count;
    size_t *order;
    size_t cursor;
    size_t last;
    int shuffle;
    pthread_mutex_t mu;
};

static int cmp_path(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static int ends_mp3(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static void shuffle_order(Playlist *pl)
{
    size_t i;

    for (i = 0; i < pl->count; i++) {
        pl->order[i] = i;
    }
    if (pl->shuffle && pl->count > 1) {
        for (i = pl->count - 1; i > 0; i--) {
            size_t j = (size_t)(rand() % (int)(i + 1));
            size_t tmp = pl->order[i];
            pl->order[i] = pl->order[j];
            pl->order[j] = tmp;
        }
        if (pl->last != (size_t)-1 && pl->order[0] == pl->last) {
            size_t tmp = pl->order[0];
            pl->order[0] = pl->order[1];
            pl->order[1] = tmp;
        }
    }
    pl->cursor = 0;
}

Playlist *playlist_load(const char *folder, int shuffle)
{
    DIR *dir;
    struct dirent *de;
    Playlist *pl;
    size_t cap = 0;

    dir = opendir(folder);
    if (!dir) {
        log_error("autodj", "cannot open folder %s", folder);
        return NULL;
    }

    pl = calloc(1, sizeof *pl);
    if (!pl) {
        closedir(dir);
        return NULL;
    }
    pl->shuffle = shuffle;
    pl->last = (size_t)-1;
    pthread_mutex_init(&pl->mu, NULL);
    srand((unsigned)time(NULL));

    while ((de = readdir(dir)) != NULL) {
        char path[4096];
        struct stat st;
        int n;

        if (de->d_name[0] == '.') {
            continue;
        }
        if (!ends_mp3(de->d_name)) {
            continue;
        }
        n = snprintf(path, sizeof path, "%s/%s", folder, de->d_name);
        if (n < 0 || (size_t)n >= sizeof path) {
            log_warn("autodj", "path too long, skipping %s", de->d_name);
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (pl->count == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char **np = realloc(pl->paths, ncap * sizeof *np);
            if (!np) {
                log_error("autodj", "out of memory reading %s", folder);
                closedir(dir);
                playlist_free(pl);
                return NULL;
            }
            pl->paths = np;
            cap = ncap;
        }
        pl->paths[pl->count] = strdup(path);
        if (!pl->paths[pl->count]) {
            closedir(dir);
            playlist_free(pl);
            return NULL;
        }
        pl->count++;
    }
    closedir(dir);

    if (pl->count == 0) {
        log_error("autodj", "no mp3 files in %s", folder);
        playlist_free(pl);
        return NULL;
    }

    qsort(pl->paths, pl->count, sizeof pl->paths[0], cmp_path);
    pl->order = calloc(pl->count, sizeof *pl->order);
    if (!pl->order) {
        playlist_free(pl);
        return NULL;
    }
    shuffle_order(pl);
    log_info("autodj", "%zu tracks, shuffle %s", pl->count, shuffle ? "on" : "off");
    return pl;
}

void playlist_free(Playlist *pl)
{
    size_t i;

    if (!pl) {
        return;
    }
    for (i = 0; i < pl->count; i++) {
        free(pl->paths[i]);
    }
    free(pl->paths);
    free(pl->order);
    pthread_mutex_destroy(&pl->mu);
    free(pl);
}

size_t playlist_count(const Playlist *pl)
{
    return pl ? pl->count : 0;
}

int playlist_next(Playlist *pl, char *dest, size_t destlen)
{
    size_t idx;

    if (!pl || pl->count == 0 || destlen == 0) {
        return -1;
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->cursor >= pl->count) {
        shuffle_order(pl);
    }
    idx = pl->order[pl->cursor++];
    pl->last = idx;
    snprintf(dest, destlen, "%s", pl->paths[idx]);
    pthread_mutex_unlock(&pl->mu);
    return 0;
}
