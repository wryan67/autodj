#ifndef AUTODJ_CONFIG_H
#define AUTODJ_CONFIG_H

#include <stddef.h>

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
    int loudness;      /* 1 to normalize each track */
    int loudness_lufs; /* target integrated loudness */
    int key_pause;
    int key_skip;
    int key_prev;
    int key_rew05;
    int key_rew10;
    int key_rew15;
    int key_vol_up;
    int key_vol_dn;
    char key_pause_name[64];
    char key_skip_name[64];
    char key_prev_name[64];
    char key_rew05_name[64];
    char key_rew10_name[64];
    char key_rew15_name[64];
    char key_vol_up_name[64];
    char key_vol_dn_name[64];
} Config;

/* Load path into cfg. Missing optional keys keep their defaults.
 * Returns 0 on success. */
int config_load(const char *path, Config *cfg);

/* Write volume (percent, 0-100) into the [Main] section of path.
 * Every other line is kept. Returns 0 on success. */
int config_save_volume(const char *path, int percent);

/* Text the config editor shows. Bool and number fields stay text so a bad
 * value from the file can be corrected instead of silently replaced. */
typedef struct ConfigEdit {
    char folder[AUTODJ_PATH];
    char shuffle[32];
    char overlap[32];
    char trim_silence[32];
    char pause_time[32];
    char resume_time[32];
    char volume[32];
    char loudness[32];
    char loudness_lufs[32];
    char key_pause[64];
    char key_skip[64];
    char key_prev[64];
    char key_rew05[64];
    char key_rew10[64];
    char key_rew15[64];
    char key_vol_up[64];
    char key_vol_dn[64];
} ConfigEdit;

/* Returns 0 and sets *out when s is true/false, yes/no, on/off, or 1/0. */
int config_parse_bool(const char *s, int *out);

/* Load path for editing. A missing file yields defaults and returns 0.
 * Folder may be empty. Returns -1 when the file exists but cannot be read. */
int config_load_edit(const char *path, ConfigEdit *edit);

/* Parse edit into cfg. On failure writes one message per line into err and
 * leaves cfg unchanged. Returns 0 when the values can be saved. */
int config_validate_edit(const ConfigEdit *edit, Config *cfg, char *err, size_t err_n);

/* Rewrite known settings in path. Comments and unknown lines stay.
 * Creates parent directories and the file when they are missing. */
int config_save(const char *path, const Config *cfg, char *err, size_t err_n);

int config_self_test(void);

#endif
