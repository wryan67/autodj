#include "playlist.h"

#include "log.h"

#include <dirent.h>
#include <errno.h>
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
    size_t *order;        /* cache mode: order[0] is the next song */
    size_t cursor;        /* in-memory mode */
    size_t last;
    int shuffle;
    size_t *queue;        /* songs asked for on the command line, in order */
    size_t queued;
    size_t queue_pos;
    unsigned char *skip;  /* queued this run */
    int cache_mode;
    int deferred;         /* command-line songs already moved to the bottom */
    int dirty;
    int save_warned;
    char *cache_path;
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

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
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

static void rotate_head(Playlist *pl)
{
    size_t head;

    if (pl->count == 0) {
        return;
    }
    head = pl->order[0];
    if (pl->count > 1) {
        memmove(pl->order, pl->order + 1, (pl->count - 1) * sizeof *pl->order);
    }
    pl->order[pl->count - 1] = head;
    pl->last = head;
    if (pl->cache_mode) {
        pl->dirty = 1;
    }
}

/* Move songs requested on the command line to the bottom, once, keeping
 * the order of the other songs and the order of the requested songs. */
static void defer_cli(Playlist *pl)
{
    size_t *fresh;
    size_t n = 0;
    size_t i;
    int any = 0;

    if (pl->deferred) {
        return;
    }
    pl->deferred = 1;
    for (i = 0; i < pl->count; i++) {
        if (pl->skip[i]) {
            any = 1;
        }
    }
    if (!any) {
        return;
    }
    fresh = malloc(pl->count * sizeof *fresh);
    if (!fresh) {
        log_error("autodj", "out of memory");
        memset(pl->skip, 0, pl->count);
        return;
    }
    for (i = 0; i < pl->count; i++) {
        if (!pl->skip[pl->order[i]]) {
            fresh[n++] = pl->order[i];
        }
    }
    if (n == 0) {
        free(fresh);
        memset(pl->skip, 0, pl->count);
        return;
    }
    for (i = 0; i < pl->count; i++) {
        if (pl->skip[pl->order[i]]) {
            fresh[n++] = pl->order[i];
        }
    }
    memcpy(pl->order, fresh, pl->count * sizeof *fresh);
    free(fresh);
    memset(pl->skip, 0, pl->count);
    pl->dirty = 1;
}

static int cache_select(Playlist *pl, size_t *idx, int consume)
{
    if (pl->count == 0) {
        return -1;
    }
    if (pl->queue_pos < pl->queued) {
        *idx = pl->queue[pl->queue_pos];
        if (consume) {
            pl->last = *idx;
            pl->queue_pos++;
        }
        return 0;
    }
    defer_cli(pl);
    *idx = pl->order[0];
    if (consume) {
        rotate_head(pl);
    }
    return 0;
}

static int nocache_take(Playlist *pl, size_t *idx_out)
{
    size_t idx;

    if (pl->queue_pos < pl->queued) {
        idx = pl->queue[pl->queue_pos++];
        pl->last = idx;
        *idx_out = idx;
        return 0;
    }
    do {
        if (pl->cursor >= pl->count) {
            shuffle_order(pl);
        }
        idx = pl->order[pl->cursor++];
    } while (pl->skip[idx] && pl->cursor < pl->count);
    if (pl->skip[idx]) {
        shuffle_order(pl);
        idx = pl->order[pl->cursor++];
    }
    pl->last = idx;
    *idx_out = idx;
    return 0;
}

static int nocache_peek(Playlist *pl, size_t *idx_out)
{
    size_t c;

    if (pl->queue_pos < pl->queued) {
        *idx_out = pl->queue[pl->queue_pos];
        return 0;
    }
    if (pl->cursor >= pl->count) {
        shuffle_order(pl);
        *idx_out = pl->order[0];
        return 0;
    }
    c = pl->cursor;
    while (c < pl->count && pl->skip[pl->order[c]]) {
        c++;
    }
    if (c >= pl->count) {
        shuffle_order(pl);
        *idx_out = pl->order[0];
        return 0;
    }
    *idx_out = pl->order[c];
    return 0;
}

static void nocache_started(Playlist *pl, const char *path)
{
    size_t idx;
    size_t c;
    size_t got;

    if (pl->queue_pos < pl->queued) {
        idx = pl->queue[pl->queue_pos];
        if (strcmp(pl->paths[idx], path) == 0) {
            pl->last = idx;
            pl->queue_pos++;
        }
        return;
    }
    if (pl->cursor >= pl->count) {
        return;
    }
    c = pl->cursor;
    while (c < pl->count && pl->skip[pl->order[c]]) {
        c++;
    }
    if (c >= pl->count) {
        return;
    }
    if (strcmp(pl->paths[pl->order[c]], path) != 0) {
        return;
    }
    if (nocache_take(pl, &got) != 0) {
        return;
    }
}

static int find_base(const Playlist *pl, const char *base, size_t *idx)
{
    size_t i;

    for (i = 0; i < pl->count; i++) {
        if (strcmp(base_name(pl->paths[i]), base) == 0) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static void shuffle_ids(size_t *ids, size_t n)
{
    size_t i;

    if (n < 2) {
        return;
    }
    for (i = n - 1; i > 0; i--) {
        size_t j = (size_t)(rand() % (int)(i + 1));
        size_t tmp = ids[i];
        ids[i] = ids[j];
        ids[j] = tmp;
    }
}

static int merge_cache(Playlist *pl, size_t *kept_out, size_t *added_out, size_t *dropped_out)
{
    FILE *fp;
    char line[4096];
    unsigned char *seen;
    size_t *missing;
    size_t norder = 0;
    size_t nmiss = 0;
    size_t kept = 0;
    size_t dropped = 0;
    size_t i;

    seen = calloc(pl->count, 1);
    missing = malloc(pl->count * sizeof *missing);
    if (!seen || !missing) {
        free(seen);
        free(missing);
        return -1;
    }
    fp = fopen(pl->cache_path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            log_warn("autodj", "cannot read %s: %s", pl->cache_path, strerror(errno));
        }
    } else {
        while (fgets(line, sizeof line, fp)) {
            size_t n = strlen(line);
            const char *base;
            const char *slash;
            size_t idx;
            int split = 0;

            if (n == sizeof line - 1 && line[n - 1] != '\n') {
                int ch;
                split = 1;
                while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                }
            }
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
                line[--n] = '\0';
            }
            if (split || n == 0) {
                continue;
            }
            base = line;
            slash = strrchr(line, '/');
            if (slash && slash[1]) {
                base = slash + 1;
            }
            if (!find_base(pl, base, &idx)) {
                dropped++;
                continue;
            }
            if (seen[idx]) {
                continue;
            }
            seen[idx] = 1;
            pl->order[norder++] = idx;
            kept++;
        }
        fclose(fp);
    }
    for (i = 0; i < pl->count; i++) {
        if (!seen[i]) {
            missing[nmiss++] = i;
        }
    }
    free(seen);
    if (pl->shuffle) {
        shuffle_ids(missing, nmiss);
    }
    for (i = 0; i < nmiss; i++) {
        pl->order[norder++] = missing[i];
    }
    free(missing);
    if (norder != pl->count) {
        return -1;
    }
    pl->dirty = 1;
    *kept_out = kept;
    *added_out = nmiss;
    *dropped_out = dropped;
    return 0;
}

static int mkdir_p(char *dir)
{
    size_t len = strlen(dir);
    size_t i;

    if (len == 0) {
        return -1;
    }
    for (i = 1; i < len; i++) {
        if (dir[i] != '/') {
            continue;
        }
        dir[i] = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            dir[i] = '/';
            return -1;
        }
        dir[i] = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ensure_parent(const char *path)
{
    char dir[4096];
    size_t n = strlen(path);
    char *slash;

    if (n == 0 || n >= sizeof dir) {
        return -1;
    }
    memcpy(dir, path, n + 1);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return 0;
    }
    *slash = '\0';
    return mkdir_p(dir);
}

static char *queue_text(const Playlist *pl, size_t *len_out)
{
    size_t len = 0;
    size_t i;
    char *buf;
    char *p;

    for (i = 0; i < pl->count; i++) {
        len += strlen(base_name(pl->paths[pl->order[i]])) + 1;
    }
    buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    p = buf;
    for (i = 0; i < pl->count; i++) {
        const char *name = base_name(pl->paths[pl->order[i]]);
        size_t n = strlen(name);

        memcpy(p, name, n);
        p += n;
        *p++ = '\n';
    }
    *p = '\0';
    *len_out = (size_t)(p - buf);
    return buf;
}

static int write_cache(const char *path, const char *text, size_t len)
{
    char tmp[4104];
    FILE *fp;
    int n;

    if (ensure_parent(path) != 0) {
        return -1;
    }
    n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return -1;
    }
    fp = fopen(tmp, "w");
    if (!fp) {
        return -1;
    }
    if (len > 0 && fwrite(text, 1, len, fp) != len) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static Playlist *load_folder(const char *folder, int shuffle, const char *cache_path)
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
    if (cache_path && cache_path[0]) {
        size_t kept = 0;
        size_t added = 0;
        size_t dropped = 0;

        pl->cache_mode = 1;
        pl->cache_path = strdup(cache_path);
        if (!pl->cache_path || merge_cache(pl, &kept, &added, &dropped) != 0) {
            log_error("autodj", "out of memory reading %s", cache_path);
            playlist_free(pl);
            return NULL;
        }
        log_info("autodj", "%zu tracks, shuffle %s", pl->count, shuffle ? "on" : "off");
        log_info("autodj", "songs.cache %s, kept %zu, added %zu, dropped %zu",
                 pl->cache_path, kept, added, dropped);
        playlist_flush(pl);
    } else {
        shuffle_order(pl);
        log_info("autodj", "%zu tracks, shuffle %s", pl->count, shuffle ? "on" : "off");
    }
    return pl;
}

Playlist *playlist_load(const char *folder, int shuffle)
{
    return load_folder(folder, shuffle, NULL);
}

Playlist *playlist_open(const char *folder, int shuffle, const char *cache_path)
{
    return load_folder(folder, shuffle, cache_path);
}

void playlist_flush(Playlist *pl)
{
    char *text = NULL;

    if (!pl || !pl->cache_path) {
        return;
    }
    for (;;) {
        size_t len = 0;
        int rc;

        pthread_mutex_lock(&pl->mu);
        if (!pl->dirty) {
            pthread_mutex_unlock(&pl->mu);
            free(text);
            return;
        }
        free(text);
        text = queue_text(pl, &len);
        if (!text) {
            pthread_mutex_unlock(&pl->mu);
            if (!pl->save_warned) {
                log_warn("autodj", "out of memory saving %s", pl->cache_path);
                pl->save_warned = 1;
            }
            return;
        }
        pl->dirty = 0;
        pthread_mutex_unlock(&pl->mu);
        rc = write_cache(pl->cache_path, text, len);
        if (rc != 0) {
            pthread_mutex_lock(&pl->mu);
            pl->dirty = 1;
            pthread_mutex_unlock(&pl->mu);
            if (!pl->save_warned) {
                log_warn("autodj", "cannot save %s: %s", pl->cache_path, strerror(errno));
                pl->save_warned = 1;
            }
            free(text);
            return;
        }
        pl->save_warned = 0;
    }
}

void playlist_free(Playlist *pl)
{
    size_t i;

    if (!pl) {
        return;
    }
    playlist_flush(pl);
    for (i = 0; i < pl->count; i++) {
        free(pl->paths[i]);
    }
    free(pl->paths);
    free(pl->order);
    free(pl->queue);
    free(pl->skip);
    free(pl->cache_path);
    pthread_mutex_destroy(&pl->mu);
    free(pl);
}

size_t playlist_count(const Playlist *pl)
{
    return pl ? pl->count : 0;
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
    int do_flush = 0;

    if (!pl || pl->count == 0 || destlen == 0) {
        return -1;
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->cache_mode) {
        if (cache_select(pl, &idx, 1) != 0) {
            pthread_mutex_unlock(&pl->mu);
            return -1;
        }
        do_flush = pl->dirty;
    } else if (nocache_take(pl, &idx) != 0) {
        pthread_mutex_unlock(&pl->mu);
        return -1;
    }
    snprintf(dest, destlen, "%s", pl->paths[idx]);
    pthread_mutex_unlock(&pl->mu);
    if (do_flush) {
        playlist_flush(pl);
    }
    return 0;
}

int playlist_peek(Playlist *pl, char *dest, size_t destlen)
{
    size_t idx;

    if (!pl || pl->count == 0 || destlen == 0) {
        return -1;
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->cache_mode) {
        if (cache_select(pl, &idx, 0) != 0) {
            pthread_mutex_unlock(&pl->mu);
            return -1;
        }
    } else if (nocache_peek(pl, &idx) != 0) {
        pthread_mutex_unlock(&pl->mu);
        return -1;
    }
    snprintf(dest, destlen, "%s", pl->paths[idx]);
    pthread_mutex_unlock(&pl->mu);
    return 0;
}

void playlist_started(Playlist *pl, const char *path)
{
    if (!pl || !path || !path[0]) {
        return;
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->cache_mode) {
        if (pl->queue_pos < pl->queued) {
            size_t idx = pl->queue[pl->queue_pos];

            if (strcmp(pl->paths[idx], path) == 0) {
                pl->last = idx;
                pl->queue_pos++;
            }
        } else {
            defer_cli(pl);
            if (strcmp(pl->paths[pl->order[0]], path) == 0) {
                rotate_head(pl);
            }
        }
    } else {
        nocache_started(pl, path);
    }
    pthread_mutex_unlock(&pl->mu);
}

static int next_is(Playlist *pl, const char *want)
{
    char got[4096];

    return playlist_next(pl, got, sizeof got) == 0 && strcmp(base_name(got), want) == 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (!fp) {
        return -1;
    }
    if (fputs(text, fp) < 0) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int cache_line(const char *path, size_t which, char *dest, size_t destlen)
{
    FILE *fp;
    char line[512];
    size_t n = 0;

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (n == which) {
            snprintf(dest, destlen, "%s", line);
            fclose(fp);
            return 0;
        }
        n++;
    }
    fclose(fp);
    return -1;
}

static int cache_self_test(const char *dir)
{
    char cache[512];
    char got[4096];
    char line[256];
    Playlist *pl = NULL;
    int rc = -1;

    snprintf(cache, sizeof cache, "%s/songs.cache", dir);
    if (write_text(cache,
                   "Wheels.mp3\n"
                   "Baby Shark.mp3\n"
                   "gone.mp3\n"
                   "\n"
                   "/tmp/nope/Bananas.MP3\n"
                   "Wheels.mp3\n") != 0) {
        log_error("playlist", "cannot write a test cache");
        return -1;
    }
    pl = playlist_open(dir, 1, cache);
    if (!pl || playlist_count(pl) != 5) {
        log_error("playlist", "cached folder did not load 5 songs");
        goto out;
    }
    if (cache_line(cache, 0, line, sizeof line) != 0 || strcmp(line, "Wheels.mp3") != 0 ||
        cache_line(cache, 1, line, sizeof line) != 0 || strcmp(line, "Baby Shark.mp3") != 0 ||
        cache_line(cache, 2, line, sizeof line) != 0 || strcmp(line, "Bananas.MP3") != 0) {
        log_error("playlist", "saved order was not kept");
        goto out;
    }
    if (!next_is(pl, "Wheels.mp3") || !next_is(pl, "Baby Shark.mp3") ||
        !next_is(pl, "Bananas.MP3")) {
        log_error("playlist", "saved order was not played");
        goto out;
    }
    if (playlist_next(pl, got, sizeof got) != 0) {
        log_error("playlist", "missing songs were not added");
        goto out;
    }
    {
        int first_beluga = strcmp(base_name(got), "baby beluga.mp3") == 0;
        int first_007 = strcmp(base_name(got), "007-wheels-again.mp3") == 0;
        int pair = 0;

        if (playlist_next(pl, got, sizeof got) != 0) {
            log_error("playlist", "missing songs were not added");
            goto out;
        }
        if (first_beluga) {
            pair = strcmp(base_name(got), "007-wheels-again.mp3") == 0;
        } else if (first_007) {
            pair = strcmp(base_name(got), "baby beluga.mp3") == 0;
        }
        if (!pair) {
            log_error("playlist", "new songs were not the ones missing from songs.cache");
            goto out;
        }
    }
    playlist_free(pl);
    pl = NULL;
    {
        int i;
        int found[5] = {0, 0, 0, 0, 0};
        static const char *want[] = {
            "Wheels.mp3", "Baby Shark.mp3", "Bananas.MP3",
            "baby beluga.mp3", "007-wheels-again.mp3"
        };

        for (i = 0; i < 5; i++) {
            int k;

            if (cache_line(cache, (size_t)i, line, sizeof line) != 0) {
                log_error("playlist", "songs.cache does not list 5 songs");
                goto out;
            }
            if (strcmp(line, "gone.mp3") == 0) {
                log_error("playlist", "a missing file stayed in songs.cache");
                goto out;
            }
            for (k = 0; k < 5; k++) {
                if (strcmp(line, want[k]) == 0) {
                    found[k]++;
                }
            }
        }
        for (i = 0; i < 5; i++) {
            if (found[i] != 1) {
                log_error("playlist", "songs.cache lost or repeated %s", want[i]);
                goto out;
            }
        }
        if (cache_line(cache, 5, line, sizeof line) == 0) {
            log_error("playlist", "songs.cache has an extra line");
            goto out;
        }
    }

    if (write_text(cache,
                   "Baby Shark.mp3\n"
                   "Wheels.mp3\n"
                   "Bananas.MP3\n"
                   "baby beluga.mp3\n"
                   "007-wheels-again.mp3\n") != 0) {
        log_error("playlist", "cannot rewrite the test cache");
        goto out;
    }
    pl = playlist_open(dir, 1, cache);
    if (!pl || playlist_queue_prefix(pl, "Wheels") != 2) {
        log_error("playlist", "cached list did not match the command-line songs");
        goto out;
    }
    if (!next_is(pl, "007-wheels-again.mp3") || !next_is(pl, "Wheels.mp3")) {
        log_error("playlist", "command-line songs did not play first");
        goto out;
    }
    if (!next_is(pl, "Baby Shark.mp3") || !next_is(pl, "Bananas.MP3") ||
        !next_is(pl, "baby beluga.mp3") || !next_is(pl, "Wheels.mp3") ||
        !next_is(pl, "007-wheels-again.mp3")) {
        log_error("playlist", "command-line songs were not moved to the bottom");
        goto out;
    }
    playlist_free(pl);
    pl = NULL;

    if (write_text(cache, "Wheels.mp3\nBaby Shark.mp3\n") != 0) {
        log_error("playlist", "cannot write a short test cache");
        goto out;
    }
    pl = playlist_open(dir, 0, cache);
    if (!pl || !next_is(pl, "Wheels.mp3") || !next_is(pl, "Baby Shark.mp3") ||
        !next_is(pl, "007-wheels-again.mp3") || !next_is(pl, "Bananas.MP3") ||
        !next_is(pl, "baby beluga.mp3")) {
        log_error("playlist", "new songs were not added in order");
        goto out;
    }
    playlist_free(pl);
    pl = NULL;

    remove(cache);
    pl = playlist_open(dir, 0, cache);
    if (!pl || !next_is(pl, "007-wheels-again.mp3") || !next_is(pl, "Baby Shark.mp3") ||
        !next_is(pl, "Bananas.MP3") || !next_is(pl, "Wheels.mp3") ||
        !next_is(pl, "baby beluga.mp3")) {
        log_error("playlist", "a new songs.cache was not alphabetical");
        goto out;
    }
    playlist_free(pl);
    pl = NULL;

    if (write_text(cache,
                   "Baby Shark.mp3\n"
                   "Wheels.mp3\n"
                   "Bananas.MP3\n") != 0) {
        goto out;
    }
    pl = playlist_open(dir, 0, cache);
    if (!pl || playlist_peek(pl, got, sizeof got) != 0 ||
        strcmp(base_name(got), "Baby Shark.mp3") != 0) {
        log_error("playlist", "peek did not show the next song");
        goto out;
    }
    playlist_started(pl, "/not/the/song.mp3");
    playlist_flush(pl);
    if (cache_line(cache, 0, line, sizeof line) != 0 || strcmp(line, "Baby Shark.mp3") != 0) {
        log_error("playlist", "a different song moved the list");
        goto out;
    }
    playlist_started(pl, got);
    playlist_flush(pl);
    if (cache_line(cache, 0, line, sizeof line) != 0 || strcmp(line, "Wheels.mp3") != 0) {
        log_error("playlist", "the song that started stayed at the top");
        goto out;
    }
    if (playlist_peek(pl, got, sizeof got) != 0 || strcmp(base_name(got), "Wheels.mp3") != 0) {
        log_error("playlist", "the next song was not at the top");
        goto out;
    }
    playlist_free(pl);
    pl = playlist_open(dir, 0, cache);
    if (!pl || !next_is(pl, "Wheels.mp3")) {
        log_error("playlist", "songs.cache was not read back");
        goto out;
    }
    rc = 0;
out:
    playlist_free(pl);
    remove(cache);
    return rc;
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
    playlist_free(pl);
    pl = playlist_load(dir, 0);
    if (!pl || playlist_peek(pl, got, sizeof got) != 0 ||
        strcmp(base_name(got), "007-wheels-again.mp3") != 0) {
        log_error("playlist", "peek did not show the first song");
        goto out;
    }
    playlist_started(pl, "/somewhere/else.mp3");
    if (playlist_peek(pl, got, sizeof got) != 0 ||
        strcmp(base_name(got), "007-wheels-again.mp3") != 0) {
        log_error("playlist", "a different song advanced the list");
        goto out;
    }
    playlist_started(pl, got);
    if (playlist_peek(pl, got, sizeof got) != 0 ||
        strcmp(base_name(got), "Baby Shark.mp3") != 0) {
        log_error("playlist", "the song that started stayed next");
        goto out;
    }
    playlist_free(pl);
    pl = NULL;
    if (cache_self_test(dir) != 0) {
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
