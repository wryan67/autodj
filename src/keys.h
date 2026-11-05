#ifndef AUTODJ_KEYS_H
#define AUTODJ_KEYS_H

struct Config;

/* Resolve a KEY_* name from linux/input-event-codes.h. Returns 0 on success. */
int key_from_name(const char *name, int *code);
const char *key_name(int code);

/* Open and exclusively grab the USB keypad. Returns 0, or -1 if no keypad. */
int keys_open(void);
void keys_close(void);
int keys_fd(void);

/* Grab one device. Returns 0, or -1 if it stays shared (errno is unchanged). */
int keys_grab(int fd);
void keys_ungrab(int fd);

/* Open keyboards for a one-shot capture. The USB keypad that reports KEY_KP0
 * is fds[0] when present, and *keypad is set to 1. Other keyboards follow.
 * Devices are not grabbed. Returns the number opened. */
int keys_open_capture(int *fds, int max, int *keypad);
void keys_close_capture(int *fds, int n);

/* First new key press pending on fd, or -1. Repeats and KEY_NUMLOCK are skipped. */
int keys_read_press(int fd);

/* Drain pending events and dispatch the actions in cfg. */
void keys_poll(const struct Config *cfg);

int keys_self_test(void);

#endif
