#ifndef AUTODJ_CONFIG_H
#define AUTODJ_CONFIG_H

#define AUTODJ_PATH 4096

typedef struct Config {
    char folder[AUTODJ_PATH];
    int shuffle;
    int overlap_sec;
    int trim_silence;
    int pause_sec;
    int resume_sec;
    int volume;     /* percent, 0-100 */
    int volume_set; /* 1 when the file contained volume */
    int key_pause;
    int key_skip;
    int key_vol_up;
    int key_vol_dn;
    char key_pause_name[64];
    char key_skip_name[64];
    char key_vol_up_name[64];
    char key_vol_dn_name[64];
} Config;

/* Load path into cfg. Missing optional keys keep their defaults.
 * Returns 0 on success. */
int config_load(const char *path, Config *cfg);

/* Write volume (percent, 0-100) into the [Main] section of path.
 * Every other line is kept. Returns 0 on success. */
int config_save_volume(const char *path, int percent);

int config_self_test(void);

#endif
