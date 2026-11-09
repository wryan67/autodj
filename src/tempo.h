#ifndef AUTODJ_TEMPO_H
#define AUTODJ_TEMPO_H

#include <stddef.h>

/* Beats per minute of interleaved stereo pcm, or 0 when no steady beat is
 * found. Looks at up to two minutes from the middle of the song.
 */
double tempo_detect(const float *pcm, size_t frames, unsigned rate);

#endif
