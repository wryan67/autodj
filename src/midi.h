#ifndef AUTODJ_MIDI_H
#define AUTODJ_MIDI_H

#include <stddef.h>

struct Config;

/*
 * MIDI controllers such as the Akai MPD218, read from the ALSA raw MIDI
 * devices /dev/snd/midiC*D*. A pad is NOTE_<n>. A knob is CC_<n>_UP when it
 * turns clockwise and CC_<n>_DOWN when it turns counterclockwise. The MIDI
 * channel and the device are not part of the name, so any controller that
 * sends the same note or CC runs the same action.
 */
enum {
    MIDI_NOTE = 0x100,
    MIDI_CC_UP = 0x200,
    MIDI_CC_DOWN = 0x300,
    MIDI_KIND_MASK = 0x300,
    MIDI_NUM_MASK = 0x7f
};

/* Parse NOTE_36, CC_3_UP, or CC_3_DOWN, in any letter case. Returns 0. */
int midi_from_name(const char *name, int *code);
/* Name of a code from midi_from_name, written into buf. */
const char *midi_name(int code, char *buf, size_t n);
/* Canonical upper-case name. dest may be name. Returns 0, or -1 when the
 * name is not a MIDI control. */
int midi_canon(const char *name, char *dest, size_t n);
/* 1 for a knob code (CC_<n>_UP or CC_<n>_DOWN). */
int midi_is_knob(int code);

/* Open every MIDI input that is not a virtual port. Returns the count. */
int midi_open(void);
void midi_close(void);
/* Copy the open fds into fds. Returns the count. */
int midi_fds(int *fds, int max);
/* Read pending messages and run the actions bound in cfg. Devices that
 * were unplugged are dropped, and new ones are opened every 3 seconds. */
void midi_poll(const struct Config *cfg);

/* Open every MIDI input for a one-shot capture in the editor. names gets
 * the device names, separated by ", ", and *busy the number of inputs that
 * another program, such as a running autodj, holds. Returns the count. */
int midi_open_capture(int *fds, int max, char *names, size_t names_n, int *busy);
void midi_close_capture(int *fds, int n);
/* First pad press or knob turn pending on fd, or -1. When the device is
 * gone, fd is closed and -2 is returned. */
int midi_read_press(int fd);

int midi_self_test(void);

#endif
