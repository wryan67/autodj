#ifndef AUTODJ_AUDIO_H
#define AUTODJ_AUDIO_H

#include <stdint.h>

/* Open the default playback device. *rate is the requested rate and is
 * replaced with the rate the device accepted. Returns 0 on success. */
int audio_open(unsigned *rate);
void audio_close(void);

/* Frames of interleaved stereo the device wants per write. */
unsigned audio_period(void);

/* Write interleaved S16 stereo. frames is the number of frame pairs. */
int audio_write(const int16_t *interleaved, unsigned frames);

int audio_self_test(void);

#endif
