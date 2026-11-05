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

/* Drain pending events and dispatch the actions in cfg. */
void keys_poll(const struct Config *cfg);

int keys_self_test(void);

#endif
