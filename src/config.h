#ifndef AUTODJ_CONFIG_H
#define AUTODJ_CONFIG_H

#include "fx.h"

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
    int dial_volume;   /* 1 when the dial starts as a volume knob */
    int key_pause;
    int key_skip;
    int key_prev;
    int key_rew05;
    int key_rew10;
    int key_rew15;
    int key_dial_fwd;
    int key_dial_back;
    int key_dial_mode;
    int key_reverb;
    int key_pitch;
    int key_autopan;
    int key_phaser;
    int key_chorus;
    int key_flanger;
    char key_pause_name[64];
    char key_skip_name[64];
    char key_prev_name[64];
    char key_rew05_name[64];
    char key_rew10_name[64];
    char key_rew15_name[64];
    char key_dial_fwd_name[64];
    char key_dial_back_name[64];
    char key_dial_mode_name[64];
    char key_reverb_name[64];
    char key_pitch_name[64];
    char key_autopan_name[64];
    char key_phaser_name[64];
    char key_chorus_name[64];
    char key_flanger_name[64];
    FxParams fx;
    /* [Jog] */
    double jog_turn_sec;   /* audio moved by one turn of the dial */
    int jog_clicks;        /* dial clicks in one turn */
    double jog_max_speed;  /* fastest scratch, times normal speed */
    double jog_spinup_sec; /* back to normal speed after the dial stops */
    double jog_grab_sec;   /* normal playback held back when the dial starts */
    double jog_smooth_sec; /* how quickly the speed follows the dial */
} Config;

/* Range for each [Jog] setting. */
#define JOG_TURN_MIN 0.1
#define JOG_TURN_MAX 20.0
#define JOG_CLICKS_MIN 1
#define JOG_CLICKS_MAX 1000
#define JOG_SPEED_MIN 0.5
#define JOG_SPEED_MAX 20.0
#define JOG_SPINUP_MAX 5.0
#define JOG_GRAB_MAX 1.0
#define JOG_SMOOTH_MAX 1.0

/* Put the [Jog] defaults into cfg. */
void config_jog_defaults(Config *cfg);

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
    char dial_default[32];
    char key_pause[64];
    char key_skip[64];
    char key_prev[64];
    char key_rew05[64];
    char key_rew10[64];
    char key_rew15[64];
    char key_dial_fwd[64];
    char key_dial_back[64];
    char key_dial_mode[64];
    char key_reverb[64];
    char key_pitch[64];
    char key_autopan[64];
    char key_phaser[64];
    char key_chorus[64];
    char key_flanger[64];
    char rv_decay[32];
    char rv_bw[32];
    char rv_damp[32];
    char rv_send[32];
    char ps_pitch[32];
    char ps_range[32];
    char ps_semi[32];
    char ps_formant[32];
    char ap_period[32];
    char ap_smooth[32];
    char ap_distance[32];
    char ph_period[32];
    char ph_low[32];
    char ph_high[32];
    char ph_stages[32];
    char ph_feedback[32];
    char ph_mix[32];
    char ph_boost[32];
    char ch_period[32];
    char ch_delay[32];
    char ch_depth[32];
    char ch_voices[32];
    char ch_mix[32];
    char fl_period[32];
    char fl_delay[32];
    char fl_depth[32];
    char fl_feedback[32];
    char fl_mix[32];
} ConfigEdit;

/* Returns 0 and sets *out when s is true/false, yes/no, on/off, or 1/0. */
int config_parse_bool(const char *s, int *out);

/* Returns 0 and sets *volume when s is volume (1) or jog (0). */
int config_parse_dial(const char *s, int *volume);

/* Load path for editing. A missing file yields defaults and returns 0.
 * Folder may be empty. Returns -1 when the file exists but cannot be read. */
int config_load_edit(const char *path, ConfigEdit *edit);

/* Parse edit into cfg. On failure writes one message per line into err and
 * leaves cfg unchanged. Returns 0 when the values can be saved. */
int config_validate_edit(const ConfigEdit *edit, Config *cfg, char *err, size_t err_n);

/* Rewrite known settings in path. Comments and unknown lines stay.
 * Creates parent directories and the file when they are missing. */
int config_save(const char *path, const Config *cfg, char *err, size_t err_n);
/* Write only the [Jog] settings of cfg into path, keeping every other line. */
int config_save_jog(const char *path, const Config *cfg, char *err, size_t err_n);

int config_self_test(void);

#endif
