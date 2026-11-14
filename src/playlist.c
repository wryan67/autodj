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
#include <unistd.h>

struct Playlist {
    char **paths;
    size_t count;
    size_t *order;
    size_t cursor;
    size_t last;
    int shuffle;
    size_t *queue;        /* songs asked for on the command line, in order */
    size_t queued;
    size_t queue_pos;
    unsigned char *skip;  /* queued, so left out of this pass of order */
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
        if (pl->skip) {
            pl->skip[i] = 0;
        }
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
    pl->queue = calloc(pl->count, sizeof *pl->queue);
    pl->skip = calloc(pl->count, 1);
    if (!pl->order || !pl->queue || !pl->skip) {
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
    free(pl->queue);
    free(pl->skip);
    pthread_mutex_destroy(&pl->mu);
    free(pl);
}

size_t playlist_count(const Playlist *pl)
{
    return pl ? pl->count : 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

/* qsort has no context argument. Set under the playlist lock. */
static char **g_sort_paths;

static int cmp_queued(const void *a, const void *b)
{
    const size_t *ia = a;
    const size_t *ib = b;
    int c = strcasecmp(base_name(g_sort_paths[*ia]), base_name(g_sort_paths[*ib]));

    return c ? c : strcmp(g_sort_paths[*ia], g_sort_paths[*ib]);
}

/* The name after a leading track number, such as "001-" or "01 . ", or
 * the whole name when it has none. */
static const char *after_track_number(const char *name)
{
    const char *p = name;

    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (p == name) {
        return name;
    }
    while (*p == '-' || *p == '_' || *p == '.' || *p == ' ') {
        p++;
    }
    return *p ? p : name;
}

int playlist_queue_prefix(Playlist *pl, const char *prefix)
{
    size_t len;
    size_t start;
    size_t i;
    int matched = 0;

    if (!pl || !prefix) {
        return 0;
    }
    prefix = base_name(prefix);
    len = strlen(prefix);
    if (len == 0) {
        return 0;
    }
    pthread_mutex_lock(&pl->mu);
    start = pl->queued;
    for (i = 0; i < pl->count; i++) {
        size_t k;
        int dup = 0;
        const char *name = base_name(pl->paths[i]);

        if (strncasecmp(name, prefix, len) != 0 &&
            strncasecmp(after_track_number(name), prefix, len) != 0) {
            continue;
        }
        matched++;
        for (k = pl->queue_pos; k < pl->queued; k++) {
            if (pl->queue[k] == i) {
                dup = 1;
            }
        }
        if (!dup && pl->queue_pos > 0 && pl->queue_pos == pl->queued) {
            /* Everything queued has played. Reuse the space. */
            pl->queue_pos = 0;
            pl->queued = 0;
            start = 0;
        }
        if (!dup && pl->queued < pl->count) {
            pl->queue[pl->queued++] = i;
            pl->skip[i] = 1;
        }
    }
    g_sort_paths = pl->paths;
    qsort(pl->queue + start, pl->queued - start, sizeof pl->queue[0], cmp_queued);
    pthread_mutex_unlock(&pl->mu);
    return matched;
}

int playlist_next(Playlist *pl, char *dest, size_t destlen)
{
    size_t idx;

    if (!pl || pl->count == 0 || destlen == 0) {
        return -1;
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->queue_pos < pl->queued) {
        idx = pl->queue[pl->queue_pos++];
        pl->last = idx;
        snprintf(dest, destlen, "%s", pl->paths[idx]);
        pthread_mutex_unlock(&pl->mu);
        return 0;
    }
    do {
        if (pl->cursor >= pl->count) {
            shuffle_order(pl);
        }
        idx = pl->order[pl->cursor++];
    } while (pl->skip[idx] && pl->cursor < pl->count);
    if (pl->skip[idx]) {
        /* Every song was queued. Start a fresh pass. */
        shuffle_order(pl);
        idx = pl->order[pl->cursor++];
    }
    pl->last = idx;
    snprintf(dest, destlen, "%s", pl->paths[idx]);
    pthread_mutex_unlock(&pl->mu);
    return 0;
}

static int next_is(Playlist *pl, const char *want)
{
    char got[4096];

    return playlist_next(pl, got, sizeof got) == 0 && strcmp(base_name(got), want) == 0;
}

int playlist_self_test(void)
{
    static const char *names[] = {
        "Baby Shark.mp3", "baby beluga.mp3", "Bananas.MP3", "Wheels.mp3", "notes.txt",
        "007-wheels-again.mp3"
    };
    char dir[] = "/tmp/autodj-songs-XXXXXX";
    char path[256];
    char got[4096];
    Playlist *pl = NULL;
    size_t i;
    int rc = -1;

    if (!mkdtemp(dir)) {
        log_error("playlist", "cannot create a temp folder");
        return -1;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        FILE *fp;

        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        fp = fopen(path, "w");
        if (fp) {
            fclose(fp);
        }
    }
    pl = playlist_load(dir, 1);
    if (!pl || playlist_count(pl) != 5) {
        log_error("playlist", "test folder did not load 5 songs");
        goto out;
    }
    /* Letter case does not matter, and every match plays, in name order. */
    if (playlist_queue_prefix(pl, "BABY") != 2 || playlist_queue_prefix(pl, "ban") != 1 ||
        playlist_queue_prefix(pl, "baby s") != 1 || playlist_queue_prefix(pl, "x") != 0 ||
        playlist_queue_prefix(pl, "notes") != 0) {
        log_error("playlist", "song names did not match");
        goto out;
    }
    if (!next_is(pl, "baby beluga.mp3") || !next_is(pl, "Baby Shark.mp3") ||
        !next_is(pl, "Bananas.MP3")) {
        log_error("playlist", "the songs asked for did not play first, in order");
        goto out;
    }
    /* The rest of the first pass leaves out the songs that already played. */
    for (i = 0; i < 2; i++) {
        if (playlist_next(pl, got, sizeof got) != 0 ||
            (strcmp(base_name(got), "Wheels.mp3") != 0 &&
             strcmp(base_name(got), "007-wheels-again.mp3") != 0)) {
            log_error("playlist", "a song asked for played again in the first pass");
            goto out;
        }
    }
    for (i = 0; i < 8; i++) {
        if (playlist_next(pl, got, sizeof got) != 0) {
            log_error("playlist", "the normal order did not continue");
            goto out;
        }
    }
    playlist_free(pl);
    /* A folder in front of the name is ignored, and so is a track number. */
    pl = playlist_load(dir, 0);
    if (!pl || playlist_queue_prefix(pl, "/some/where/whe") != 2 ||
        !next_is(pl, "007-wheels-again.mp3") || !next_is(pl, "Wheels.mp3") ||
        playlist_queue_prefix(pl, "007") != 1 || !next_is(pl, "007-wheels-again.mp3") ||
        playlist_queue_prefix(pl, "WHEELS-A") != 1) {
        log_error("playlist", "a song name with a folder or track number did not match");
        goto out;
    }
    rc = 0;
out:
    playlist_free(pl);
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        remove(path);
    }
    rmdir(dir);
    return rc;
}
