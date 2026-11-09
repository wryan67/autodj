#ifndef AUTODJ_DECODE_H
#define AUTODJ_DECODE_H

#include <stddef.h>

typedef struct Track {
    float *pcm; /* interleaved stereo at `rate` */
    size_t frames;
    size_t start; /* first audible frame */
    size_t end;   /* one past the last audible frame */
    unsigned rate;
    double bpm; /* 0 when no steady beat was found */
    char name[256];
    char path[4096];
} Track;

int decode_init(void);
void decode_shutdown(void);

/* Decode path to stereo PCM at `rate`. Silence outside [start, end) is
 * trimmed when trim is non-zero. When loudness is non-zero, the whole file
 * is measured and gained toward loudness_lufs. Returns NULL on failure. */
Track *track_decode(const char *path, unsigned rate, int trim, int loudness, int loudness_lufs);
void track_free(Track *track);

int trim_self_test(void);
int loudness_self_test(void);
int decode_self_test(const char *path);

#endif
