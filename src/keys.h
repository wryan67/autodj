#ifndef AUTODJ_KEYS_H
#define AUTODJ_KEYS_H

struct Config;

/* Resolve a KEY_* name from linux/input-event-codes.h, optionally prefixed
 * by CTRL+, SHIFT+, ALT+, or META+ (for example CTRL+SHIFT+KEY_Z). The
 * modifiers are stored above bit 16 of the code. Returns 0 on success. */
int key_from_name(const char *name, int *code);
/* Name for a code from key_from_name. A name with modifiers is kept in a
 * small rotating buffer, so copy it before the next few calls. */
const char *key_name(int code);

/* Open and exclusively grab the USB keypad and the XP-Pen shortcut remote.
 * Returns 0, or -1 if neither is present. */
int keys_open(void);
void keys_close(void);
/* How many devices keys_open grabbed. Zero while another program, such
 * as a running autodj, holds them. */
int keys_grabbed(void);
/* Also call fn for each dial click that keys_poll passes to player_jog.
 * NULL removes it. */
void keys_set_jog_hook(void (*fn)(int direction, long long when_us));
/* Copy the open device fds into fds. Returns the count. */
int keys_fds(int *fds, int max);

/* Grab one device. Returns 0, or -1 if it stays shared (errno is unchanged). */
int keys_grab(int fd);
void keys_ungrab(int fd);

/* Open keyboards for a one-shot capture. The keypads (USB keypad, shortcut
 * remote) come first and *keypad is set to how many there are. Other
 * keyboards follow. Devices are not grabbed. Returns the number opened. */
int keys_open_capture(int *fds, int max, int *keypad);
void keys_close_capture(int *fds, int n);

/* First new key press pending on fd, or -1, with any held modifiers.
 * Repeats and KEY_NUMLOCK are skipped. Modifier state is kept per fd. */
int keys_read_press(int fd);

/* Drain pending events and dispatch the actions in cfg.
 * A press within 30 ms of the previous press is ignored. */
void keys_poll(const struct Config *cfg);

int keys_self_test(void);

#endif
