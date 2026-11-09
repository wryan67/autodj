#ifndef AUTODJ_CFGEDIT_H
#define AUTODJ_CFGEDIT_H

/* Edit the config at path. Returns 0 on save or cancel, -1 on error. */
int config_edit(const char *path);

/* Full-screen view to tune and test the jog dial settings in [Jog] of the
 * config at path. Returns 0 on save or cancel, -1 on error. */
int dial_calibrate(const char *path);

int cfgedit_self_test(void);

#endif
