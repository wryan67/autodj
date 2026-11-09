#include "config.h"

#include "keys.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    KEYID_PAUSE = 0,
    KEYID_SKIP,
    KEYID_PREV,
    KEYID_REW05,
    KEYID_REW10,
    KEYID_REW15,
    KEYID_DIAL_FWD,
    KEYID_DIAL_BACK,
    KEYID_DIAL_MODE,
    KEYID_REVERB,
    KEYID_PITCH,
    KEYID_AUTOPAN,
    KEYID_PHASER,
    KEYID_COUNT
};

static void trim(char *s)
{
    char *start;
    char *end;
    size_t n;

    start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    n = strlen(s);
    end = s + n;
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_bool(const char *s, int *out)
{
    if (eq_ci(s, "true") || eq_ci(s, "yes") || eq_ci(s, "on") || strcmp(s, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (eq_ci(s, "false") || eq_ci(s, "no") || eq_ci(s, "off") || strcmp(s, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int config_parse_bool(const char *s, int *out)
{
    if (!s || !out) {
        return -1;
    }
    return parse_bool(s, out);
}

static int parse_nonneg(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (!s[0]) {
        return -1;
    }
    v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 86400) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_real(const char *s, double min, double max, double *out)
{
    char *end = NULL;
    double v;

    if (!s || !s[0]) {
        return -1;
    }
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || !end || *end != '\0' || v != v || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

void config_jog_defaults(Config *cfg)
{
    cfg->jog_turn_sec = 1.8;
    cfg->jog_clicks = 20;
    cfg->jog_max_speed = 4.0;
    cfg->jog_spinup_sec = 0.3;
    cfg->jog_grab_sec = 0.02;
    cfg->jog_smooth_sec = 0.01;
}

static void apply_jog(Config *cfg, const char *key, const char *val)
{
    double v = 0;

    if (eq_ci(key, "seconds_per_turn")) {
        if (parse_real(val, JOG_TURN_MIN, JOG_TURN_MAX, &v) != 0) {
            log_warn("autodj", "seconds_per_turn must be from 0.1 to 20, got %s", val);
        } else {
            cfg->jog_turn_sec = v;
        }
    } else if (eq_ci(key, "clicks_per_turn")) {
        if (parse_real(val, JOG_CLICKS_MIN, JOG_CLICKS_MAX, &v) != 0 || v != (int)v) {
            log_warn("autodj", "clicks_per_turn must be a whole number from 1 to 1000, got %s", val);
        } else {
            cfg->jog_clicks = (int)v;
        }
    } else if (eq_ci(key, "max_speed")) {
        if (parse_real(val, JOG_SPEED_MIN, JOG_SPEED_MAX, &v) != 0) {
            log_warn("autodj", "max_speed must be from 0.5 to 20, got %s", val);
        } else {
            cfg->jog_max_speed = v;
        }
    } else if (eq_ci(key, "spinup_time")) {
        if (parse_real(val, 0, JOG_SPINUP_MAX, &v) != 0) {
            log_warn("autodj", "spinup_time must be from 0 to 5 seconds, got %s", val);
        } else {
            cfg->jog_spinup_sec = v;
        }
    } else if (eq_ci(key, "grab_time")) {
        if (parse_real(val, 0, JOG_GRAB_MAX, &v) != 0) {
            log_warn("autodj", "grab_time must be from 0 to 1 second, got %s", val);
        } else {
            cfg->jog_grab_sec = v;
        }
    } else if (eq_ci(key, "smoothing")) {
        if (parse_real(val, 0, JOG_SMOOTH_MAX, &v) != 0) {
            log_warn("autodj", "smoothing must be from 0 to 1 second, got %s", val);
        } else {
            cfg->jog_smooth_sec = v;
        }
    } else {
        log_warn("autodj", "unknown jog setting %s", key);
    }
}

static void set_key(Config *cfg, const char *action, const char *keyname)
{
    int code = -1;

    if (eq_ci(action, "volume_up") || eq_ci(action, "brake_delayvolume_up") ||
        eq_ci(action, "volume_dn") || eq_ci(action, "volume_down")) {
        /* The dial sets the volume. Old volume keys are left unused. */
        return;
    }
    if (eq_ci(action, "pitch_shift") && cfg->key_pitch >= 0) {
        /* A chipmunk line wins over the old pitch_shift name. */
        return;
    }
    if (key_from_name(keyname, &code) != 0) {
        log_warn("autodj", "unknown key name %s for %s", keyname, action);
        return;
    }
    if (eq_ci(action, "pause_resume")) {
        cfg->key_pause = code;
        snprintf(cfg->key_pause_name, sizeof cfg->key_pause_name, "%s", keyname);
    } else if (eq_ci(action, "skip")) {
        cfg->key_skip = code;
        snprintf(cfg->key_skip_name, sizeof cfg->key_skip_name, "%s", keyname);
    } else if (eq_ci(action, "previous")) {
        cfg->key_prev = code;
        snprintf(cfg->key_prev_name, sizeof cfg->key_prev_name, "%s", keyname);
    } else if (eq_ci(action, "rewind_0_5")) {
        cfg->key_rew05 = code;
        snprintf(cfg->key_rew05_name, sizeof cfg->key_rew05_name, "%s", keyname);
    } else if (eq_ci(action, "rewind_1")) {
        cfg->key_rew10 = code;
        snprintf(cfg->key_rew10_name, sizeof cfg->key_rew10_name, "%s", keyname);
    } else if (eq_ci(action, "rewind_1_5")) {
        cfg->key_rew15 = code;
        snprintf(cfg->key_rew15_name, sizeof cfg->key_rew15_name, "%s", keyname);
    } else if (eq_ci(action, "reverb")) {
        cfg->key_reverb = code;
        snprintf(cfg->key_reverb_name, sizeof cfg->key_reverb_name, "%s", keyname);
    } else if (eq_ci(action, "chipmunk") || eq_ci(action, "pitch_shift")) {
        /* pitch_shift is the old name of this button. */
        cfg->key_pitch = code;
        snprintf(cfg->key_pitch_name, sizeof cfg->key_pitch_name, "%s", keyname);
    } else if (eq_ci(action, "autopan")) {
        cfg->key_autopan = code;
        snprintf(cfg->key_autopan_name, sizeof cfg->key_autopan_name, "%s", keyname);
    } else if (eq_ci(action, "phaser")) {
        cfg->key_phaser = code;
        snprintf(cfg->key_phaser_name, sizeof cfg->key_phaser_name, "%s", keyname);
    } else if (eq_ci(action, "dial_forward") || eq_ci(action, "jog_forward")) {
        cfg->key_dial_fwd = code;
        snprintf(cfg->key_dial_fwd_name, sizeof cfg->key_dial_fwd_name, "%s", keyname);
    } else if (eq_ci(action, "dial_back") || eq_ci(action, "jog_back")) {
        cfg->key_dial_back = code;
        snprintf(cfg->key_dial_back_name, sizeof cfg->key_dial_back_name, "%s", keyname);
    } else if (eq_ci(action, "dial_mode")) {
        cfg->key_dial_mode = code;
        snprintf(cfg->key_dial_mode_name, sizeof cfg->key_dial_mode_name, "%s", keyname);
    } else {
        log_warn("autodj", "unknown key action %s", action);
    }
}

static int key_code_used(const Config *cfg, int code)
{
    if (code < 0) {
        return 0;
    }
    return cfg->key_pause == code || cfg->key_skip == code || cfg->key_prev == code ||
           cfg->key_rew05 == code || cfg->key_rew10 == code || cfg->key_rew15 == code ||
           cfg->key_dial_fwd == code || cfg->key_dial_back == code ||
           cfg->key_dial_mode == code || cfg->key_reverb == code ||
           cfg->key_pitch == code || cfg->key_autopan == code ||
           cfg->key_phaser == code;
}

/* A missing previous or rewind binding takes the keypad default, unless
 * that key is already used by another action. */
static void bind_default(Config *cfg, int *slot, const char *action, const char *keyname)
{
    int code = -1;

    if (!slot || *slot >= 0) {
        return;
    }
    if (key_from_name(keyname, &code) != 0 || key_code_used(cfg, code)) {
        return;
    }
    set_key(cfg, action, keyname);
}

static int fx_section(const char *section)
{
    if (eq_ci(section, "Reverb")) {
        return 0;
    }
    /* Pitch Shift is the old name of the chipmunk section. */
    if (eq_ci(section, "Chipmunk") || eq_ci(section, "Pitch Shift") ||
        eq_ci(section, "PitchShift")) {
        return 1;
    }
    if (eq_ci(section, "Autopan")) {
        return 2;
    }
    if (eq_ci(section, "Phaser")) {
        return 3;
    }
    return -1;
}

/* Keys left from the old Mixxx-style phaser. They are ignored and dropped. */
static int old_phaser_key(const char *key)
{
    return eq_ci(key, "depth") || eq_ci(key, "range") || eq_ci(key, "triplets") ||
           eq_ci(key, "stereo");
}

static void apply_fx(Config *cfg, int section, const char *key, const char *val)
{
    double v = 0;
    int on = 0;
    const char *what = section == 0 ? "reverb" : section == 1 ? "chipmunk" :
                       section == 2 ? "autopan" : "phaser";

    if (eq_ci(key, "on")) {
        /* Old configs stored the startup state here. Effects always start off. */
        return;
    }
    if (section == 3 && old_phaser_key(key)) {
        return;
    }
    if ((section == 1 && eq_ci(key, "semitones")) ||
        (section == 1 && eq_ci(key, "formant"))) {
        if (parse_bool(val, &on) != 0) {
            log_warn("autodj", "%s %s must be true or false, got %s", what, key, val);
            return;
        }
        if (eq_ci(key, "semitones")) {
            cfg->fx.ps_semi = on;
        } else {
            cfg->fx.ps_formant = on;
        }
        return;
    }
    if (section == 0 && eq_ci(key, "decay")) {
        if (parse_real(val, 0, 1, &v) != 0) {
            log_warn("autodj", "reverb decay must be from 0 to 1, got %s", val);
        } else {
            cfg->fx.rv_decay = (float)v;
        }
    } else if (section == 0 && (eq_ci(key, "bw") || eq_ci(key, "bandwidth"))) {
        if (parse_real(val, 0, 1, &v) != 0) {
            log_warn("autodj", "reverb bw must be from 0 to 1, got %s", val);
        } else {
            cfg->fx.rv_bw = (float)v;
        }
    } else if (section == 0 && eq_ci(key, "damping")) {
        if (parse_real(val, 0, 1, &v) != 0) {
            log_warn("autodj", "reverb damping must be from 0 to 1, got %s", val);
        } else {
            cfg->fx.rv_damp = (float)v;
        }
    } else if (section == 0 && eq_ci(key, "send")) {
        if (parse_real(val, 0, 1, &v) != 0) {
            log_warn("autodj", "reverb send must be from 0 to 1, got %s", val);
        } else {
            cfg->fx.rv_send = (float)v;
        }
    } else if (section == 1 && eq_ci(key, "pitch")) {
        if (parse_real(val, -1, 1, &v) != 0) {
            log_warn("autodj", "chipmunk pitch must be from -1 to 1, got %s", val);
        } else {
            cfg->fx.ps_pitch = (float)v;
        }
    } else if (section == 1 && eq_ci(key, "range")) {
        if (parse_real(val, 0, 2, &v) != 0) {
            log_warn("autodj", "chipmunk range must be from 0 to 2, got %s", val);
        } else {
            cfg->fx.ps_range = (float)v;
        }
    } else if (section == 2 && eq_ci(key, "period")) {
        if (parse_real(val, 0.25, 16, &v) != 0) {
            log_warn("autodj", "autopan period must be from 0.25 to 16, got %s", val);
        } else {
            cfg->fx.ap_period = (float)v;
        }
    } else if (section == 2 && eq_ci(key, "smoothing")) {
        if (parse_real(val, 0.25, 0.5, &v) != 0) {
            log_warn("autodj", "autopan smoothing must be from 0.25 to 0.5, got %s", val);
        } else {
            cfg->fx.ap_smooth = (float)v;
        }
    } else if (section == 2 && eq_ci(key, "distance")) {
        if (parse_real(val, 0, 100, &v) != 0) {
            log_warn("autodj", "autopan distance must be from 0 to 100, got %s", val);
        } else {
            cfg->fx.ap_distance = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "period")) {
        if (parse_real(val, 0.25, 16, &v) != 0) {
            log_warn("autodj", "phaser period must be from 0.25 to 16, got %s", val);
        } else {
            cfg->fx.ph_period = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "low")) {
        if (parse_real(val, 20, 20000, &v) != 0) {
            log_warn("autodj", "phaser low must be from 20 to 20000, got %s", val);
        } else {
            cfg->fx.ph_low = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "high")) {
        if (parse_real(val, 20, 20000, &v) != 0) {
            log_warn("autodj", "phaser high must be from 20 to 20000, got %s", val);
        } else {
            cfg->fx.ph_high = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "stages")) {
        if (parse_real(val, 1, 12, &v) != 0 || v != (int)v) {
            log_warn("autodj", "phaser stages must be a whole number from 1 to 12, got %s", val);
        } else {
            cfg->fx.ph_stages = (int)v;
        }
    } else if (section == 3 && eq_ci(key, "feedback")) {
        if (parse_real(val, 0, 0.9, &v) != 0) {
            log_warn("autodj", "phaser feedback must be from 0 to 0.9, got %s", val);
        } else {
            cfg->fx.ph_feedback = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "mix")) {
        if (parse_real(val, 0, 100, &v) != 0) {
            log_warn("autodj", "phaser mix must be from 0 to 100, got %s", val);
        } else {
            cfg->fx.ph_blend = (float)v;
        }
    } else if (section == 3 && eq_ci(key, "boost")) {
        if (parse_real(val, 0, 100, &v) != 0) {
            log_warn("autodj", "phaser boost must be from 0 to 100, got %s", val);
        } else {
            cfg->fx.ph_boost = (float)v;
        }
    } else {
        log_warn("autodj", "unknown %s setting %s", what, key);
    }
}

static void apply(Config *cfg, const char *section, const char *key, const char *val)
{
    if (eq_ci(section, "Main")) {
        if (eq_ci(key, "Folder")) {
            snprintf(cfg->folder, sizeof cfg->folder, "%s", val);
        } else if (eq_ci(key, "shuffle")) {
            if (parse_bool(val, &cfg->shuffle) != 0) {
                log_warn("autodj", "shuffle must be true or false, got %s", val);
            }
        } else if (eq_ci(key, "overlap")) {
            if (parse_nonneg(val, &cfg->overlap_sec) != 0) {
                log_warn("autodj", "overlap must be a number of seconds, got %s", val);
            }
        } else if (eq_ci(key, "trim_silence")) {
            if (parse_bool(val, &cfg->trim_silence) != 0) {
                log_warn("autodj", "trim_silence must be true or false, got %s", val);
            }
        } else if (eq_ci(key, "pause_time")) {
            if (parse_nonneg(val, &cfg->pause_sec) != 0) {
                log_warn("autodj", "pause_time must be a number of seconds, got %s", val);
            }
        } else if (eq_ci(key, "resume_time")) {
            if (parse_nonneg(val, &cfg->resume_sec) != 0) {
                log_warn("autodj", "resume_time must be a number of seconds, got %s", val);
            }
        } else if (eq_ci(key, "volume")) {
            int percent = 0;
            if (parse_nonneg(val, &percent) != 0 || percent > 100) {
                log_warn("autodj", "volume must be a percent from 0 to 100, got %s", val);
            } else {
                cfg->volume = percent;
                cfg->volume_set = 1;
            }
        } else if (eq_ci(key, "loudness")) {
            if (parse_bool(val, &cfg->loudness) != 0) {
                log_warn("autodj", "loudness must be true or false, got %s", val);
            }
        } else if (eq_ci(key, "dial_default")) {
            if (config_parse_dial(val, &cfg->dial_volume) != 0) {
                log_warn("autodj", "dial_default must be volume or jog, got %s", val);
            }
        } else if (eq_ci(key, "loudness_lufs")) {
            char *end = NULL;
            long v;

            v = strtol(val, &end, 10);
            if (!val[0] || !end || *end != '\0' || v < -30 || v > -5) {
                log_warn("autodj", "loudness_lufs must be from -30 to -5, got %s", val);
            } else {
                cfg->loudness_lufs = (int)v;
            }
        } else {
            log_warn("autodj", "unknown setting %s", key);
        }
        return;
    }
    if (eq_ci(section, "Keys")) {
        set_key(cfg, key, val);
        return;
    }
    if (eq_ci(section, "Jog")) {
        apply_jog(cfg, key, val);
        return;
    }
    {
        int fx = fx_section(section);

        if (fx >= 0) {
            apply_fx(cfg, fx, key, val);
            return;
        }
    }
    log_warn("autodj", "ignoring %s in section %s", key, section[0] ? section : "(none)");
}

int config_load(const char *path, Config *cfg)
{
    FILE *fp;
    char line[AUTODJ_PATH + 128];
    char section[64];

    memset(cfg, 0, sizeof *cfg);
    cfg->shuffle = 1;
    cfg->overlap_sec = 10;
    cfg->trim_silence = 1;
    cfg->pause_sec = 5;
    cfg->resume_sec = 5;
    cfg->volume = 100;
    cfg->volume_set = 0;
    cfg->loudness = 1;
    cfg->loudness_lufs = -18;
    cfg->dial_volume = 1;
    cfg->key_pause = -1;
    cfg->key_skip = -1;
    cfg->key_prev = -1;
    cfg->key_rew05 = -1;
    cfg->key_rew10 = -1;
    cfg->key_rew15 = -1;
    cfg->key_dial_fwd = -1;
    cfg->key_dial_back = -1;
    cfg->key_dial_mode = -1;
    cfg->key_reverb = -1;
    cfg->key_pitch = -1;
    cfg->key_autopan = -1;
    cfg->key_phaser = -1;
    config_jog_defaults(cfg);
    fx_defaults(&cfg->fx);
    section[0] = '\0';

    fp = fopen(path, "r");
    if (!fp) {
        log_error("autodj", "cannot open config %s", path);
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        char *eq;

        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end || end == line + 1) {
                log_warn("autodj", "bad section line %s", line);
                continue;
            }
            *end = '\0';
            {
                size_t n = strlen(line + 1);
                if (n >= sizeof section) {
                    n = sizeof section - 1;
                }
                memcpy(section, line + 1, n);
                section[n] = '\0';
            }
            trim(section);
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            log_warn("autodj", "ignoring line without '=': %s", line);
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        apply(cfg, section, line, eq + 1);
    }
    fclose(fp);

    if (!cfg->folder[0]) {
        log_error("autodj", "config %s has no Folder", path);
        return -1;
    }
    bind_default(cfg, &cfg->key_prev, "previous", "KEY_KPSLASH");
    bind_default(cfg, &cfg->key_rew05, "rewind_0_5", "KEY_KP1");
    bind_default(cfg, &cfg->key_rew10, "rewind_1", "KEY_KP2");
    bind_default(cfg, &cfg->key_rew15, "rewind_1_5", "KEY_KP3");
    cfg->fx.reverb_on = 0;
    cfg->fx.pitch_on = 0;
    cfg->fx.autopan_on = 0;
    cfg->fx.phaser_on = 0;
    if (cfg->fx.ph_low >= cfg->fx.ph_high) {
        FxParams def;

        fx_defaults(&def);
        log_warn("autodj", "phaser low (%g) must be below high (%g), using %g and %g",
                 (double)cfg->fx.ph_low, (double)cfg->fx.ph_high,
                 (double)def.ph_low, (double)def.ph_high);
        cfg->fx.ph_low = def.ph_low;
        cfg->fx.ph_high = def.ph_high;
    }
    return 0;
}

struct CfgLine {
    char *text;
    struct CfgLine *next;
};

static void free_lines(struct CfgLine *head)
{
    while (head) {
        struct CfgLine *next = head->next;
        free(head->text);
        free(head);
        head = next;
    }
}

static int append_line(struct CfgLine **head, struct CfgLine **tail, const char *text)
{
    struct CfgLine *line = calloc(1, sizeof *line);

    if (!line) {
        return -1;
    }
    line->text = strdup(text);
    if (!line->text) {
        free(line);
        return -1;
    }
    if (*tail) {
        (*tail)->next = line;
    } else {
        *head = line;
    }
    *tail = line;
    return 0;
}

static int section_name(const char *line, char *name, size_t name_n)
{
    char buf[AUTODJ_PATH + 128];
    char *end;
    size_t n;

    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    if (buf[0] != '[') {
        return 0;
    }
    end = strchr(buf, ']');
    if (!end || end == buf + 1) {
        return 0;
    }
    *end = '\0';
    n = strlen(buf + 1);
    if (n >= name_n) {
        n = name_n - 1;
    }
    memcpy(name, buf + 1, n);
    name[n] = '\0';
    trim(name);
    return 1;
}

static int is_key_assignment(const char *line, const char *key)
{
    char buf[AUTODJ_PATH + 128];
    char *eq;

    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    if (buf[0] == '\0' || buf[0] == '#' || buf[0] == ';' || buf[0] == '[') {
        return 0;
    }
    eq = strchr(buf, '=');
    if (!eq) {
        return 0;
    }
    *eq = '\0';
    trim(buf);
    return eq_ci(buf, key);
}

static void write_value_line(FILE *out, const char *line, const char *key, const char *value)
{
    const char *eq = strchr(line, '=');
    const char *p;

    if (!eq) {
        fprintf(out, "%s=%s\n", key, value);
        return;
    }
    fwrite(line, 1, (size_t)(eq - line) + 1, out);
    p = eq + 1;
    while (*p == ' ' || *p == '\t') {
        fputc(*p, out);
        p++;
    }
    fprintf(out, "%s\n", value);
}

static int ends_with_newline(const char *text)
{
    size_t n = strlen(text);
    return n > 0 && text[n - 1] == '\n';
}

static int section_is(const char *name, const char *section, const char *alias)
{
    return eq_ci(name, section) || (alias && eq_ci(name, alias));
}

/* Rewrites one key in one section and leaves every other line as it is.
 * Adds the key, or the section, when it is missing.
 */
static int save_one_value(const char *path, const char *sect, const char *alias,
                          const char *key, const char *value)
{
    FILE *in;
    FILE *out = NULL;
    struct CfgLine *head = NULL;
    struct CfgLine *tail = NULL;
    struct CfgLine *sect_header = NULL;
    struct CfgLine *line;
    char buf[AUTODJ_PATH + 128];
    char tmp[AUTODJ_PATH + 16];
    char section[64];
    struct stat st;
    int fd = -1;
    int in_sect = 0;
    int found = 0;
    int created = 0;
    int rc = -1;

    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        log_error("autodj", "config path is too long");
        return -1;
    }

    in = fopen(path, "r");
    if (!in) {
        log_error("autodj", "cannot open config %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fileno(in), &st) != 0) {
        st.st_mode = 0644;
    }
    while (fgets(buf, sizeof buf, in)) {
        if (append_line(&head, &tail, buf) != 0) {
            log_error("autodj", "out of memory reading config %s", path);
            goto done;
        }
    }
    if (ferror(in)) {
        log_error("autodj", "cannot read config %s: %s", path, strerror(errno));
        goto done;
    }

    section[0] = '\0';
    for (line = head; line; line = line->next) {
        if (section_name(line->text, section, sizeof section)) {
            in_sect = section_is(section, sect, alias);
            if (in_sect && !sect_header) {
                sect_header = line;
            }
            continue;
        }
        if (in_sect && is_key_assignment(line->text, key)) {
            found = 1;
        }
    }

    fd = mkstemp(tmp);
    if (fd < 0) {
        log_error("autodj", "cannot update config %s: %s", path, strerror(errno));
        goto done;
    }
    created = 1;
    if (fchmod(fd, st.st_mode & 0777) != 0) {
        log_warn("autodj", "could not keep permissions on %s: %s", path, strerror(errno));
    }
    out = fdopen(fd, "w");
    if (!out) {
        log_error("autodj", "cannot update config %s: %s", path, strerror(errno));
        goto done;
    }
    fd = -1;

    in_sect = 0;
    for (line = head; line; line = line->next) {
        if (!found && line == sect_header) {
            fputs(line->text, out);
            if (!ends_with_newline(line->text)) {
                fputc('\n', out);
            }
            fprintf(out, "%s=%s\n", key, value);
            continue;
        }
        if (section_name(line->text, section, sizeof section)) {
            in_sect = section_is(section, sect, alias);
        }
        if (in_sect && is_key_assignment(line->text, key)) {
            write_value_line(out, line->text, key, value);
            continue;
        }
        fputs(line->text, out);
    }
    if (!found && !sect_header) {
        if (tail && !ends_with_newline(tail->text)) {
            fputc('\n', out);
        }
        fprintf(out, "[%s]\n%s=%s\n", sect, key, value);
    }
    if (ferror(out) || fflush(out) != 0 || fsync(fileno(out)) != 0) {
        log_error("autodj", "cannot write config %s: %s", path, strerror(errno));
        goto done;
    }
    fclose(out);
    out = NULL;
    fclose(in);
    in = NULL;
    if (rename(tmp, path) != 0) {
        log_error("autodj", "cannot replace config %s: %s", path, strerror(errno));
        goto done;
    }
    created = 0;
    rc = 0;

done:
    if (out) {
        fclose(out);
    } else if (fd >= 0) {
        close(fd);
    }
    if (in) {
        fclose(in);
    }
    if (created) {
        unlink(tmp);
    }
    free_lines(head);
    return rc;
}

int config_save_volume(const char *path, int percent)
{
    char value[16];

    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    snprintf(value, sizeof value, "%d", percent);
    return save_one_value(path, "Main", NULL, "volume", value);
}

static void set_err(char *err, size_t err_n, const char *fmt, ...)
{
    va_list ap;

    if (!err || err_n == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, err_n, fmt, ap);
    va_end(ap);
}

static void add_err(char *err, size_t err_n, const char *msg)
{
    size_t len;

    if (!err || err_n == 0 || !msg || !msg[0]) {
        return;
    }
    len = strlen(err);
    if (len == 0) {
        snprintf(err, err_n, "%s", msg);
        return;
    }
    if (len + 1 >= err_n) {
        return;
    }
    snprintf(err + len, err_n - len, "\n%s", msg);
}

static void edit_defaults(ConfigEdit *edit)
{
    memset(edit, 0, sizeof *edit);
    snprintf(edit->shuffle, sizeof edit->shuffle, "true");
    snprintf(edit->overlap, sizeof edit->overlap, "10");
    snprintf(edit->trim_silence, sizeof edit->trim_silence, "true");
    snprintf(edit->pause_time, sizeof edit->pause_time, "5");
    snprintf(edit->resume_time, sizeof edit->resume_time, "5");
    snprintf(edit->volume, sizeof edit->volume, "100");
    snprintf(edit->loudness, sizeof edit->loudness, "true");
    snprintf(edit->loudness_lufs, sizeof edit->loudness_lufs, "-18");
    snprintf(edit->dial_default, sizeof edit->dial_default, "volume");
    snprintf(edit->key_prev, sizeof edit->key_prev, "KEY_KPSLASH");
    snprintf(edit->key_rew05, sizeof edit->key_rew05, "KEY_KP1");
    snprintf(edit->key_rew10, sizeof edit->key_rew10, "KEY_KP2");
    snprintf(edit->key_rew15, sizeof edit->key_rew15, "KEY_KP3");
    snprintf(edit->rv_decay, sizeof edit->rv_decay, "0.5");
    snprintf(edit->rv_bw, sizeof edit->rv_bw, "1");
    snprintf(edit->rv_damp, sizeof edit->rv_damp, "0");
    snprintf(edit->rv_send, sizeof edit->rv_send, "0.5");
    snprintf(edit->ps_pitch, sizeof edit->ps_pitch, "1");
    snprintf(edit->ps_range, sizeof edit->ps_range, "1");
    snprintf(edit->ps_semi, sizeof edit->ps_semi, "true");
    snprintf(edit->ps_formant, sizeof edit->ps_formant, "false");
    snprintf(edit->ap_period, sizeof edit->ap_period, "4");
    snprintf(edit->ap_smooth, sizeof edit->ap_smooth, "0.5");
    snprintf(edit->ap_distance, sizeof edit->ap_distance, "75");
    snprintf(edit->ph_period, sizeof edit->ph_period, "1");
    snprintf(edit->ph_low, sizeof edit->ph_low, "300");
    snprintf(edit->ph_high, sizeof edit->ph_high, "3000");
    snprintf(edit->ph_stages, sizeof edit->ph_stages, "6");
    snprintf(edit->ph_feedback, sizeof edit->ph_feedback, "0.5");
    snprintf(edit->ph_mix, sizeof edit->ph_mix, "50");
    snprintf(edit->ph_boost, sizeof edit->ph_boost, "15");
}

static void copy_raw(char *dest, size_t n, const char *val)
{
    snprintf(dest, n, "%s", val);
}

static void apply_edit(ConfigEdit *edit, const char *section, const char *key, const char *val)
{
    if (eq_ci(section, "Main")) {
        if (eq_ci(key, "Folder")) {
            copy_raw(edit->folder, sizeof edit->folder, val);
        } else if (eq_ci(key, "shuffle")) {
            copy_raw(edit->shuffle, sizeof edit->shuffle, val);
        } else if (eq_ci(key, "overlap")) {
            copy_raw(edit->overlap, sizeof edit->overlap, val);
        } else if (eq_ci(key, "trim_silence")) {
            copy_raw(edit->trim_silence, sizeof edit->trim_silence, val);
        } else if (eq_ci(key, "pause_time")) {
            copy_raw(edit->pause_time, sizeof edit->pause_time, val);
        } else if (eq_ci(key, "resume_time")) {
            copy_raw(edit->resume_time, sizeof edit->resume_time, val);
        } else if (eq_ci(key, "volume")) {
            copy_raw(edit->volume, sizeof edit->volume, val);
        } else if (eq_ci(key, "loudness")) {
            copy_raw(edit->loudness, sizeof edit->loudness, val);
        } else if (eq_ci(key, "loudness_lufs")) {
            copy_raw(edit->loudness_lufs, sizeof edit->loudness_lufs, val);
        } else if (eq_ci(key, "dial_default")) {
            copy_raw(edit->dial_default, sizeof edit->dial_default, val);
        }
        return;
    }
    if (eq_ci(section, "Keys")) {
        if (eq_ci(key, "pause_resume")) {
            copy_raw(edit->key_pause, sizeof edit->key_pause, val);
        } else if (eq_ci(key, "skip")) {
            copy_raw(edit->key_skip, sizeof edit->key_skip, val);
        } else if (eq_ci(key, "previous")) {
            copy_raw(edit->key_prev, sizeof edit->key_prev, val);
        } else if (eq_ci(key, "rewind_0_5")) {
            copy_raw(edit->key_rew05, sizeof edit->key_rew05, val);
        } else if (eq_ci(key, "rewind_1")) {
            copy_raw(edit->key_rew10, sizeof edit->key_rew10, val);
        } else if (eq_ci(key, "rewind_1_5")) {
            copy_raw(edit->key_rew15, sizeof edit->key_rew15, val);
        } else if (eq_ci(key, "dial_forward") || eq_ci(key, "jog_forward")) {
            copy_raw(edit->key_dial_fwd, sizeof edit->key_dial_fwd, val);
        } else if (eq_ci(key, "dial_back") || eq_ci(key, "jog_back")) {
            copy_raw(edit->key_dial_back, sizeof edit->key_dial_back, val);
        } else if (eq_ci(key, "dial_mode")) {
            copy_raw(edit->key_dial_mode, sizeof edit->key_dial_mode, val);
        } else if (eq_ci(key, "reverb")) {
            copy_raw(edit->key_reverb, sizeof edit->key_reverb, val);
        } else if (eq_ci(key, "chipmunk")) {
            copy_raw(edit->key_pitch, sizeof edit->key_pitch, val);
        } else if (eq_ci(key, "pitch_shift") && !edit->key_pitch[0]) {
            /* The old name of the chipmunk button. */
            copy_raw(edit->key_pitch, sizeof edit->key_pitch, val);
        } else if (eq_ci(key, "autopan")) {
            copy_raw(edit->key_autopan, sizeof edit->key_autopan, val);
        } else if (eq_ci(key, "phaser")) {
            copy_raw(edit->key_phaser, sizeof edit->key_phaser, val);
        }
        return;
    }
    {
        int fx = fx_section(section);

        if (fx == 0) {
            if (eq_ci(key, "decay")) {
                copy_raw(edit->rv_decay, sizeof edit->rv_decay, val);
            } else if (eq_ci(key, "bw") || eq_ci(key, "bandwidth")) {
                copy_raw(edit->rv_bw, sizeof edit->rv_bw, val);
            } else if (eq_ci(key, "damping")) {
                copy_raw(edit->rv_damp, sizeof edit->rv_damp, val);
            } else if (eq_ci(key, "send")) {
                copy_raw(edit->rv_send, sizeof edit->rv_send, val);
            }
        } else if (fx == 1) {
            if (eq_ci(key, "pitch")) {
                copy_raw(edit->ps_pitch, sizeof edit->ps_pitch, val);
            } else if (eq_ci(key, "range")) {
                copy_raw(edit->ps_range, sizeof edit->ps_range, val);
            } else if (eq_ci(key, "semitones")) {
                copy_raw(edit->ps_semi, sizeof edit->ps_semi, val);
            } else if (eq_ci(key, "formant")) {
                copy_raw(edit->ps_formant, sizeof edit->ps_formant, val);
            }
        } else if (fx == 2) {
            if (eq_ci(key, "period")) {
                copy_raw(edit->ap_period, sizeof edit->ap_period, val);
            } else if (eq_ci(key, "smoothing")) {
                copy_raw(edit->ap_smooth, sizeof edit->ap_smooth, val);
            } else if (eq_ci(key, "distance")) {
                copy_raw(edit->ap_distance, sizeof edit->ap_distance, val);
            }
        } else if (fx == 3) {
            if (eq_ci(key, "period")) {
                copy_raw(edit->ph_period, sizeof edit->ph_period, val);
            } else if (eq_ci(key, "low")) {
                copy_raw(edit->ph_low, sizeof edit->ph_low, val);
            } else if (eq_ci(key, "high")) {
                copy_raw(edit->ph_high, sizeof edit->ph_high, val);
            } else if (eq_ci(key, "stages")) {
                copy_raw(edit->ph_stages, sizeof edit->ph_stages, val);
            } else if (eq_ci(key, "feedback")) {
                copy_raw(edit->ph_feedback, sizeof edit->ph_feedback, val);
            } else if (eq_ci(key, "mix")) {
                copy_raw(edit->ph_mix, sizeof edit->ph_mix, val);
            } else if (eq_ci(key, "boost")) {
                copy_raw(edit->ph_boost, sizeof edit->ph_boost, val);
            }
        }
    }
}

/* Drop a keypad default when the file already gave that key to another action. */
static void release_taken_default(ConfigEdit *edit, char *field, const char *defname)
{
    char *fields[13];
    int i;
    int hits = 0;

    if (strcmp(field, defname) != 0) {
        return;
    }
    fields[0] = edit->key_pause;
    fields[1] = edit->key_skip;
    fields[2] = edit->key_prev;
    fields[3] = edit->key_rew05;
    fields[4] = edit->key_rew10;
    fields[5] = edit->key_rew15;
    fields[6] = edit->key_dial_fwd;
    fields[7] = edit->key_dial_back;
    fields[8] = edit->key_dial_mode;
    fields[9] = edit->key_reverb;
    fields[10] = edit->key_pitch;
    fields[11] = edit->key_autopan;
    fields[12] = edit->key_phaser;
    for (i = 0; i < 13; i++) {
        if (strcmp(fields[i], defname) == 0) {
            hits++;
        }
    }
    if (hits > 1) {
        field[0] = '\0';
    }
}

static void release_key_defaults(ConfigEdit *edit)
{
    release_taken_default(edit, edit->key_prev, "KEY_KPSLASH");
    release_taken_default(edit, edit->key_rew05, "KEY_KP1");
    release_taken_default(edit, edit->key_rew10, "KEY_KP2");
    release_taken_default(edit, edit->key_rew15, "KEY_KP3");
}

int config_load_edit(const char *path, ConfigEdit *edit)
{
    FILE *fp;
    char line[AUTODJ_PATH + 128];
    char section[64];

    if (!path || !edit) {
        return -1;
    }
    edit_defaults(edit);
    fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            release_key_defaults(edit);
            return 0;
        }
        log_error("autodj", "cannot open config %s: %s", path, strerror(errno));
        return -1;
    }
    section[0] = '\0';
    while (fgets(line, sizeof line, fp)) {
        char *eq;

        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            size_t n;

            if (!end || end == line + 1) {
                continue;
            }
            *end = '\0';
            n = strlen(line + 1);
            if (n >= sizeof section) {
                n = sizeof section - 1;
            }
            memcpy(section, line + 1, n);
            section[n] = '\0';
            trim(section);
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        apply_edit(edit, section, line, eq + 1);
    }
    if (ferror(fp)) {
        log_error("autodj", "cannot read config %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    release_key_defaults(edit);
    return 0;
}

static int parse_int_strict(const char *s, long *out)
{
    const char *p = s;
    char *end = NULL;

    if (*p == '-') {
        p++;
    }
    if (!*p) {
        return -1;
    }
    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        p++;
    }
    errno = 0;
    *out = strtol(s, &end, 10);
    if (errno || !end || *end) {
        return -1;
    }
    return 0;
}

static int take_real(char *err, size_t err_n, const char *text, double min, double max,
                     const char *what, float *out)
{
    double v = 0;

    if (parse_real(text, min, max, &v) != 0) {
        char msg[160];

        snprintf(msg, sizeof msg, "%s must be a number from %g to %g.", what, min, max);
        add_err(err, err_n, msg);
        return -1;
    }
    *out = (float)v;
    return 0;
}

static int take_int(char *err, size_t err_n, const char *text, long min, long max,
                    const char *what, int *out)
{
    long v;

    if (parse_int_strict(text, &v) != 0 || v < min || v > max) {
        char msg[128];

        snprintf(msg, sizeof msg, "%s must be a whole number from %ld to %ld.", what, min, max);
        add_err(err, err_n, msg);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int config_parse_dial(const char *s, int *volume)
{
    if (!s || !volume) {
        return -1;
    }
    if (eq_ci(s, "volume")) {
        *volume = 1;
        return 0;
    }
    if (eq_ci(s, "jog")) {
        *volume = 0;
        return 0;
    }
    return -1;
}

static int take_bool(char *err, size_t err_n, const char *text, const char *what, int *out)
{
    if (parse_bool(text, out) != 0) {
        char msg[128];

        snprintf(msg, sizeof msg, "%s must be true or false.", what);
        add_err(err, err_n, msg);
        return -1;
    }
    return 0;
}

static int take_key(char *err, size_t err_n, const char *text, const char *what,
                    int *code, char *name, size_t name_n)
{
    if (!text[0]) {
        *code = -1;
        name[0] = '\0';
        return 0;
    }
    if (key_from_name(text, code) != 0) {
        char msg[160];

        snprintf(msg, sizeof msg, "%s key \"%s\" is not a KEY_* name.", what, text);
        add_err(err, err_n, msg);
        return -1;
    }
    snprintf(name, name_n, "%s", text);
    return 0;
}

int config_validate_edit(const ConfigEdit *edit, Config *cfg, char *err, size_t err_n)
{
    ConfigEdit in;
    Config out;
    struct stat st;
    const char *keys[KEYID_COUNT];
    const char *labels[KEYID_COUNT];
    int key_ok[KEYID_COUNT];
    int i;
    int j;
    int failed = 0;

    if (!edit || !cfg) {
        return -1;
    }
    if (err && err_n) {
        err[0] = '\0';
    }
    in = *edit;
    trim(in.folder);
    trim(in.shuffle);
    trim(in.overlap);
    trim(in.trim_silence);
    trim(in.pause_time);
    trim(in.resume_time);
    trim(in.volume);
    trim(in.loudness);
    trim(in.loudness_lufs);
    trim(in.dial_default);
    trim(in.key_pause);
    trim(in.key_skip);
    trim(in.key_prev);
    trim(in.key_rew05);
    trim(in.key_rew10);
    trim(in.key_rew15);
    trim(in.key_dial_fwd);
    trim(in.key_dial_back);
    trim(in.key_dial_mode);
    trim(in.key_reverb);
    trim(in.key_pitch);
    trim(in.key_autopan);
    trim(in.key_phaser);
    trim(in.rv_decay);
    trim(in.rv_bw);
    trim(in.rv_damp);
    trim(in.rv_send);
    trim(in.ps_pitch);
    trim(in.ps_range);
    trim(in.ps_semi);
    trim(in.ps_formant);
    trim(in.ap_period);
    trim(in.ap_smooth);
    trim(in.ap_distance);
    trim(in.ph_period);
    trim(in.ph_low);
    trim(in.ph_high);
    trim(in.ph_stages);
    trim(in.ph_feedback);
    trim(in.ph_mix);
    trim(in.ph_boost);

    memset(&out, 0, sizeof out);
    out.key_pause = -1;
    out.key_skip = -1;
    out.key_prev = -1;
    out.key_rew05 = -1;
    out.key_rew10 = -1;
    out.key_rew15 = -1;
    out.key_dial_fwd = -1;
    out.key_dial_back = -1;
    out.key_dial_mode = -1;
    out.key_reverb = -1;
    out.key_pitch = -1;
    out.key_autopan = -1;
    out.key_phaser = -1;
    fx_defaults(&out.fx);
    out.volume_set = 1;

    if (!in.folder[0]) {
        add_err(err, err_n, "Folder is required.");
        failed = 1;
    } else if (strchr(in.folder, '\n') || strchr(in.folder, '\r')) {
        add_err(err, err_n, "Folder cannot contain a line break.");
        failed = 1;
    } else if (stat(in.folder, &st) != 0) {
        if (errno == ENOENT) {
            add_err(err, err_n, "Folder does not exist.");
        } else {
            char msg[256];

            snprintf(msg, sizeof msg, "Folder cannot be opened: %s", strerror(errno));
            add_err(err, err_n, msg);
        }
        failed = 1;
    } else if (!S_ISDIR(st.st_mode)) {
        add_err(err, err_n, "Folder is not a directory.");
        failed = 1;
    } else {
        snprintf(out.folder, sizeof out.folder, "%s", in.folder);
    }

    if (take_bool(err, err_n, in.shuffle, "Shuffle", &out.shuffle) != 0) {
        failed = 1;
    }
    if (take_int(err, err_n, in.overlap, 0, 86400, "Overlap", &out.overlap_sec) != 0) {
        failed = 1;
    }
    if (take_bool(err, err_n, in.trim_silence, "Trim silence", &out.trim_silence) != 0) {
        failed = 1;
    }
    if (take_int(err, err_n, in.pause_time, 0, 86400, "Pause time", &out.pause_sec) != 0) {
        failed = 1;
    }
    if (take_int(err, err_n, in.resume_time, 0, 86400, "Resume time", &out.resume_sec) != 0) {
        failed = 1;
    }
    if (take_int(err, err_n, in.volume, 0, 100, "Volume", &out.volume) != 0) {
        failed = 1;
    }
    if (take_bool(err, err_n, in.loudness, "Loudness", &out.loudness) != 0) {
        failed = 1;
    }
    if (take_int(err, err_n, in.loudness_lufs, -30, -5, "Loudness LUFS", &out.loudness_lufs) != 0) {
        failed = 1;
    }
    out.dial_volume = 1;
    if (in.dial_default[0] && config_parse_dial(in.dial_default, &out.dial_volume) != 0) {
        add_err(err, err_n, "Dial starts in must be volume or jog.");
        failed = 1;
    }
    key_ok[KEYID_PAUSE] = take_key(err, err_n, in.key_pause, "Pause/resume",
                                   &out.key_pause, out.key_pause_name,
                                   sizeof out.key_pause_name) == 0;
    key_ok[KEYID_SKIP] = take_key(err, err_n, in.key_skip, "Skip",
                                  &out.key_skip, out.key_skip_name,
                                  sizeof out.key_skip_name) == 0;
    key_ok[KEYID_PREV] = take_key(err, err_n, in.key_prev, "Previous",
                                  &out.key_prev, out.key_prev_name,
                                  sizeof out.key_prev_name) == 0;
    key_ok[KEYID_REW05] = take_key(err, err_n, in.key_rew05, "Rewind 0.5s",
                                   &out.key_rew05, out.key_rew05_name,
                                   sizeof out.key_rew05_name) == 0;
    key_ok[KEYID_REW10] = take_key(err, err_n, in.key_rew10, "Rewind 1s",
                                   &out.key_rew10, out.key_rew10_name,
                                   sizeof out.key_rew10_name) == 0;
    key_ok[KEYID_REW15] = take_key(err, err_n, in.key_rew15, "Rewind 1.5s",
                                   &out.key_rew15, out.key_rew15_name,
                                   sizeof out.key_rew15_name) == 0;
    key_ok[KEYID_DIAL_FWD] = take_key(err, err_n, in.key_dial_fwd, "Dial forward",
                                     &out.key_dial_fwd, out.key_dial_fwd_name,
                                     sizeof out.key_dial_fwd_name) == 0;
    key_ok[KEYID_DIAL_BACK] = take_key(err, err_n, in.key_dial_back, "Dial back",
                                      &out.key_dial_back, out.key_dial_back_name,
                                      sizeof out.key_dial_back_name) == 0;
    key_ok[KEYID_DIAL_MODE] = take_key(err, err_n, in.key_dial_mode, "Dial mode",
                                      &out.key_dial_mode, out.key_dial_mode_name,
                                      sizeof out.key_dial_mode_name) == 0;
    key_ok[KEYID_REVERB] = take_key(err, err_n, in.key_reverb, "Reverb",
                                    &out.key_reverb, out.key_reverb_name,
                                    sizeof out.key_reverb_name) == 0;
    key_ok[KEYID_PITCH] = take_key(err, err_n, in.key_pitch, "Chipmunk",
                                   &out.key_pitch, out.key_pitch_name,
                                   sizeof out.key_pitch_name) == 0;
    key_ok[KEYID_AUTOPAN] = take_key(err, err_n, in.key_autopan, "Autopan",
                                     &out.key_autopan, out.key_autopan_name,
                                     sizeof out.key_autopan_name) == 0;
    key_ok[KEYID_PHASER] = take_key(err, err_n, in.key_phaser, "Phaser",
                                    &out.key_phaser, out.key_phaser_name,
                                    sizeof out.key_phaser_name) == 0;
    keys[KEYID_PAUSE] = in.key_pause;
    keys[KEYID_SKIP] = in.key_skip;
    keys[KEYID_PREV] = in.key_prev;
    keys[KEYID_REW05] = in.key_rew05;
    keys[KEYID_REW10] = in.key_rew10;
    keys[KEYID_REW15] = in.key_rew15;
    keys[KEYID_DIAL_FWD] = in.key_dial_fwd;
    keys[KEYID_DIAL_BACK] = in.key_dial_back;
    keys[KEYID_DIAL_MODE] = in.key_dial_mode;
    keys[KEYID_REVERB] = in.key_reverb;
    keys[KEYID_PITCH] = in.key_pitch;
    keys[KEYID_AUTOPAN] = in.key_autopan;
    keys[KEYID_PHASER] = in.key_phaser;
    labels[KEYID_PAUSE] = "pause/resume";
    labels[KEYID_SKIP] = "skip";
    labels[KEYID_PREV] = "previous";
    labels[KEYID_REW05] = "rewind 0.5s";
    labels[KEYID_REW10] = "rewind 1s";
    labels[KEYID_REW15] = "rewind 1.5s";
    labels[KEYID_DIAL_FWD] = "dial forward";
    labels[KEYID_DIAL_BACK] = "dial back";
    labels[KEYID_DIAL_MODE] = "dial mode";
    labels[KEYID_REVERB] = "reverb";
    labels[KEYID_PITCH] = "chipmunk";
    labels[KEYID_AUTOPAN] = "autopan";
    labels[KEYID_PHASER] = "phaser";
    if (in.rv_decay[0] && take_real(err, err_n, in.rv_decay, 0, 1, "Decay", &out.fx.rv_decay) != 0) {
        failed = 1;
    }
    if (in.rv_bw[0] && take_real(err, err_n, in.rv_bw, 0, 1, "BW", &out.fx.rv_bw) != 0) {
        failed = 1;
    }
    if (in.rv_damp[0] && take_real(err, err_n, in.rv_damp, 0, 1, "Damping", &out.fx.rv_damp) != 0) {
        failed = 1;
    }
    if (in.rv_send[0] && take_real(err, err_n, in.rv_send, 0, 1, "Send", &out.fx.rv_send) != 0) {
        failed = 1;
    }
    if (in.ps_pitch[0] && take_real(err, err_n, in.ps_pitch, -1, 1, "Pitch", &out.fx.ps_pitch) != 0) {
        failed = 1;
    }
    if (in.ps_range[0] && take_real(err, err_n, in.ps_range, 0, 2, "Pitch range", &out.fx.ps_range) != 0) {
        failed = 1;
    }
    if (in.ps_semi[0] && take_bool(err, err_n, in.ps_semi, "Semitones", &out.fx.ps_semi) != 0) {
        failed = 1;
    }
    if (in.ps_formant[0] && take_bool(err, err_n, in.ps_formant, "Formant", &out.fx.ps_formant) != 0) {
        failed = 1;
    }
    if (in.ap_period[0] &&
        take_real(err, err_n, in.ap_period, 0.25, 16, "Period", &out.fx.ap_period) != 0) {
        failed = 1;
    }
    if (in.ap_smooth[0] &&
        take_real(err, err_n, in.ap_smooth, 0.25, 0.5, "Smoothing", &out.fx.ap_smooth) != 0) {
        failed = 1;
    }
    if (in.ap_distance[0] &&
        take_real(err, err_n, in.ap_distance, 0, 100, "Distance", &out.fx.ap_distance) != 0) {
        failed = 1;
    }
    {
        int low_ok = !in.ph_low[0] ||
                     take_real(err, err_n, in.ph_low, 20, 20000, "Low Hz", &out.fx.ph_low) == 0;
        int high_ok = !in.ph_high[0] ||
                      take_real(err, err_n, in.ph_high, 20, 20000, "High Hz",
                                &out.fx.ph_high) == 0;

        if (in.ph_period[0] &&
            take_real(err, err_n, in.ph_period, 0.25, 16, "Period", &out.fx.ph_period) != 0) {
            failed = 1;
        }
        if (!low_ok || !high_ok) {
            failed = 1;
        } else if (out.fx.ph_low >= out.fx.ph_high) {
            add_err(err, err_n, "Low Hz must be below High Hz.");
            failed = 1;
        }
        if (in.ph_stages[0] &&
            take_int(err, err_n, in.ph_stages, 1, 12, "Stages", &out.fx.ph_stages) != 0) {
            failed = 1;
        }
        if (in.ph_feedback[0] &&
            take_real(err, err_n, in.ph_feedback, 0, 0.9, "Feedback",
                      &out.fx.ph_feedback) != 0) {
            failed = 1;
        }
        if (in.ph_mix[0] &&
            take_real(err, err_n, in.ph_mix, 0, 100, "Mix %", &out.fx.ph_blend) != 0) {
            failed = 1;
        }
        if (in.ph_boost[0] &&
            take_real(err, err_n, in.ph_boost, 0, 100, "Boost %", &out.fx.ph_boost) != 0) {
            failed = 1;
        }
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        if (!key_ok[i]) {
            failed = 1;
        }
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        for (j = i + 1; j < KEYID_COUNT; j++) {
            char msg[192];

            if (!key_ok[i] || !key_ok[j] || !keys[i][0] || strcmp(keys[i], keys[j]) != 0) {
                continue;
            }
            snprintf(msg, sizeof msg, "%s is used for %s and %s.", keys[i], labels[i], labels[j]);
            add_err(err, err_n, msg);
            failed = 1;
        }
    }
    if (failed) {
        return -1;
    }
    *cfg = out;
    return 0;
}

static int line_blank(const char *text)
{
    char buf[AUTODJ_PATH + 128];

    snprintf(buf, sizeof buf, "%s", text);
    trim(buf);
    return buf[0] == '\0';
}

static int line_setting_key(const char *text, char *key, size_t key_n)
{
    char buf[AUTODJ_PATH + 128];
    char *eq;

    snprintf(buf, sizeof buf, "%s", text);
    trim(buf);
    if (buf[0] == '\0' || buf[0] == '#' || buf[0] == ';' || buf[0] == '[') {
        return 0;
    }
    eq = strchr(buf, '=');
    if (!eq) {
        return 0;
    }
    *eq = '\0';
    trim(buf);
    if (!buf[0] || strlen(buf) >= key_n) {
        return 0;
    }
    memcpy(key, buf, strlen(buf) + 1);
    return 1;
}

static int main_id(const char *key)
{
    if (eq_ci(key, "Folder")) {
        return 0;
    }
    if (eq_ci(key, "shuffle")) {
        return 1;
    }
    if (eq_ci(key, "overlap")) {
        return 2;
    }
    if (eq_ci(key, "trim_silence")) {
        return 3;
    }
    if (eq_ci(key, "pause_time")) {
        return 4;
    }
    if (eq_ci(key, "resume_time")) {
        return 5;
    }
    if (eq_ci(key, "volume")) {
        return 6;
    }
    if (eq_ci(key, "loudness")) {
        return 7;
    }
    if (eq_ci(key, "loudness_lufs")) {
        return 8;
    }
    if (eq_ci(key, "dial_default")) {
        return 9;
    }
    return -1;
}

static int keys_id(const char *key)
{
    if (eq_ci(key, "pause_resume")) {
        return KEYID_PAUSE;
    }
    if (eq_ci(key, "skip")) {
        return KEYID_SKIP;
    }
    if (eq_ci(key, "previous")) {
        return KEYID_PREV;
    }
    if (eq_ci(key, "rewind_0_5")) {
        return KEYID_REW05;
    }
    if (eq_ci(key, "rewind_1")) {
        return KEYID_REW10;
    }
    if (eq_ci(key, "rewind_1_5")) {
        return KEYID_REW15;
    }
    if (eq_ci(key, "dial_forward")) {
        return KEYID_DIAL_FWD;
    }
    if (eq_ci(key, "dial_back")) {
        return KEYID_DIAL_BACK;
    }
    if (eq_ci(key, "dial_mode")) {
        return KEYID_DIAL_MODE;
    }
    if (eq_ci(key, "reverb")) {
        return KEYID_REVERB;
    }
    if (eq_ci(key, "chipmunk")) {
        return KEYID_PITCH;
    }
    if (eq_ci(key, "autopan")) {
        return KEYID_AUTOPAN;
    }
    if (eq_ci(key, "phaser")) {
        return KEYID_PHASER;
    }
    return -1;
}

static const char *g_jog_names[] = {
    "seconds_per_turn", "clicks_per_turn", "max_speed", "spinup_time", "grab_time", "smoothing"
};

enum { JOG_COUNT = 6 };

static void fmt_float(char *dest, size_t n, float v)
{
    snprintf(dest, n, "%g", (double)v);
}

static int fx_key_id(int section, const char *key)
{
    static const char *rv[] = {"decay", "bw", "damping", "send"};
    static const char *ps[] = {"pitch", "range", "semitones", "formant"};
    static const char *ap[] = {"period", "smoothing", "distance"};
    static const char *ph[] = {"period", "low", "high", "stages", "feedback", "mix", "boost"};
    const char **names = section == 0 ? rv : section == 1 ? ps : section == 2 ? ap : ph;
    int count = section == 0 ? 4 : section == 1 ? 4 : section == 2 ? 3 : 7;
    int i;

    for (i = 0; i < count; i++) {
        if (eq_ci(key, names[i]) || (section == 0 && i == 1 && eq_ci(key, "bandwidth"))) {
            return i;
        }
    }
    return -1;
}

static const char *fx_key_name(int section, int id)
{
    static const char *rv[] = {"decay", "bw", "damping", "send"};
    static const char *ps[] = {"pitch", "range", "semitones", "formant"};
    static const char *ap[] = {"period", "smoothing", "distance"};
    static const char *ph[] = {"period", "low", "high", "stages", "feedback", "mix", "boost"};

    if (section == 0) {
        return rv[id];
    }
    if (section == 1) {
        return ps[id];
    }
    if (section == 2) {
        return ap[id];
    }
    return ph[id];
}

static void fx_value(const Config *cfg, int section, int id, char *dest, size_t n)
{
    const FxParams *fx = &cfg->fx;

    if (section == 0) {
        switch (id) {
        case 0:
            fmt_float(dest, n, fx->rv_decay);
            return;
        case 1:
            fmt_float(dest, n, fx->rv_bw);
            return;
        case 2:
            fmt_float(dest, n, fx->rv_damp);
            return;
        default:
            fmt_float(dest, n, fx->rv_send);
            return;
        }
    }
    if (section == 1) {
        switch (id) {
        case 0:
            fmt_float(dest, n, fx->ps_pitch);
            return;
        case 1:
            fmt_float(dest, n, fx->ps_range);
            return;
        case 2:
            snprintf(dest, n, "%s", fx->ps_semi ? "true" : "false");
            return;
        default:
            snprintf(dest, n, "%s", fx->ps_formant ? "true" : "false");
            return;
        }
    }
    if (section == 3) {
        switch (id) {
        case 0:
            fmt_float(dest, n, fx->ph_period);
            return;
        case 1:
            fmt_float(dest, n, fx->ph_low);
            return;
        case 2:
            fmt_float(dest, n, fx->ph_high);
            return;
        case 3:
            snprintf(dest, n, "%d", fx->ph_stages);
            return;
        case 4:
            fmt_float(dest, n, fx->ph_feedback);
            return;
        case 5:
            fmt_float(dest, n, fx->ph_blend);
            return;
        default:
            fmt_float(dest, n, fx->ph_boost);
            return;
        }
    }
    switch (id) {
    case 0:
        fmt_float(dest, n, fx->ap_period);
        return;
    case 1:
        fmt_float(dest, n, fx->ap_smooth);
        return;
    default:
        fmt_float(dest, n, fx->ap_distance);
        return;
    }
}

static int jog_id(const char *key)
{
    int i;

    for (i = 0; i < JOG_COUNT; i++) {
        if (eq_ci(key, g_jog_names[i])) {
            return i;
        }
    }
    return -1;
}

static void jog_value(const Config *cfg, int id, char *dest, size_t n)
{
    switch (id) {
    case 0:
        snprintf(dest, n, "%g", cfg->jog_turn_sec);
        break;
    case 1:
        snprintf(dest, n, "%d", cfg->jog_clicks);
        break;
    case 2:
        snprintf(dest, n, "%g", cfg->jog_max_speed);
        break;
    case 3:
        snprintf(dest, n, "%g", cfg->jog_spinup_sec);
        break;
    case 4:
        snprintf(dest, n, "%g", cfg->jog_grab_sec);
        break;
    default:
        snprintf(dest, n, "%g", cfg->jog_smooth_sec);
        break;
    }
}

static void main_value(const Config *cfg, int id, char *dest, size_t n)
{
    switch (id) {
    case 0:
        snprintf(dest, n, "%s", cfg->folder);
        break;
    case 1:
        snprintf(dest, n, "%s", cfg->shuffle ? "true" : "false");
        break;
    case 2:
        snprintf(dest, n, "%d", cfg->overlap_sec);
        break;
    case 3:
        snprintf(dest, n, "%s", cfg->trim_silence ? "true" : "false");
        break;
    case 4:
        snprintf(dest, n, "%d", cfg->pause_sec);
        break;
    case 5:
        snprintf(dest, n, "%d", cfg->resume_sec);
        break;
    case 6:
        snprintf(dest, n, "%d", cfg->volume);
        break;
    case 7:
        snprintf(dest, n, "%s", cfg->loudness ? "true" : "false");
        break;
    case 8:
        snprintf(dest, n, "%d", cfg->loudness_lufs);
        break;
    default:
        snprintf(dest, n, "%s", cfg->dial_volume ? "volume" : "jog");
        break;
    }
}

static void key_value(const Config *cfg, int id, char *dest, size_t n)
{
    const char *s = "";

    switch (id) {
    case KEYID_PAUSE:
        s = cfg->key_pause_name;
        break;
    case KEYID_SKIP:
        s = cfg->key_skip_name;
        break;
    case KEYID_PREV:
        s = cfg->key_prev_name;
        break;
    case KEYID_REW05:
        s = cfg->key_rew05_name;
        break;
    case KEYID_REW10:
        s = cfg->key_rew10_name;
        break;
    case KEYID_REW15:
        s = cfg->key_rew15_name;
        break;
    case KEYID_DIAL_FWD:
        s = cfg->key_dial_fwd_name;
        break;
    case KEYID_DIAL_BACK:
        s = cfg->key_dial_back_name;
        break;
    case KEYID_DIAL_MODE:
        s = cfg->key_dial_mode_name;
        break;
    case KEYID_REVERB:
        s = cfg->key_reverb_name;
        break;
    case KEYID_PITCH:
        s = cfg->key_pitch_name;
        break;
    case KEYID_AUTOPAN:
        s = cfg->key_autopan_name;
        break;
    case KEYID_PHASER:
        s = cfg->key_phaser_name;
        break;
    default:
        break;
    }
    snprintf(dest, n, "%s", s ? s : "");
}

static int ensure_nl(struct CfgLine *line)
{
    size_t n;
    char *text;

    if (!line || ends_with_newline(line->text)) {
        return 0;
    }
    n = strlen(line->text);
    text = malloc(n + 2);
    if (!text) {
        return -1;
    }
    memcpy(text, line->text, n);
    text[n] = '\n';
    text[n + 1] = '\0';
    free(line->text);
    line->text = text;
    return 0;
}

static void unlink_line(struct CfgLine **head, struct CfgLine *prev, struct CfgLine *line)
{
    if (prev) {
        prev->next = line->next;
    } else {
        *head = line->next;
    }
    free(line->text);
    free(line);
}

static struct CfgLine *insert_after(struct CfgLine **head, struct CfgLine *after, const char *text)
{
    struct CfgLine *line = calloc(1, sizeof *line);

    if (!line) {
        return NULL;
    }
    line->text = strdup(text);
    if (!line->text) {
        free(line);
        return NULL;
    }
    if (!after) {
        line->next = *head;
        *head = line;
        return line;
    }
    if (ensure_nl(after) != 0) {
        free(line->text);
        free(line);
        return NULL;
    }
    line->next = after->next;
    after->next = line;
    return line;
}

static int rewrite_value(struct CfgLine *line, const char *value)
{
    const char *eq = strchr(line->text, '=');
    const char *p;
    char out[AUTODJ_PATH + 256];
    size_t prefix;
    size_t vlen;
    char *repl;

    if (!eq) {
        return -1;
    }
    p = eq + 1;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    prefix = (size_t)(p - line->text);
    vlen = strlen(value);
    if (prefix + vlen + 2 > sizeof out) {
        return -1;
    }
    memcpy(out, line->text, prefix);
    memcpy(out + prefix, value, vlen);
    out[prefix + vlen] = '\n';
    out[prefix + vlen + 1] = '\0';
    repl = strdup(out);
    if (!repl) {
        return -1;
    }
    free(line->text);
    line->text = repl;
    return 0;
}

static void find_section(struct CfgLine *head, const char *name,
                         struct CfgLine **header, struct CfgLine **last)
{
    struct CfgLine *line;
    char section[64];
    int in = 0;

    *header = NULL;
    *last = NULL;
    section[0] = '\0';
    for (line = head; line; line = line->next) {
        if (section_name(line->text, section, sizeof section)) {
            if (in) {
                return;
            }
            if (eq_ci(section, name)) {
                in = 1;
                *header = line;
                *last = line;
            }
            continue;
        }
        if (in && !line_blank(line->text)) {
            *last = line;
        }
    }
}

static int ensure_section(struct CfgLine **head, const char *name,
                          struct CfgLine **header, struct CfgLine **last)
{
    struct CfgLine *tail;
    char text[80];

    find_section(*head, name, header, last);
    if (*header) {
        return 0;
    }
    snprintf(text, sizeof text, "[%s]\n", name);
    tail = *head;
    while (tail && tail->next) {
        tail = tail->next;
    }
    *header = insert_after(head, tail, text);
    if (!*header) {
        return -1;
    }
    *last = *header;
    return 0;
}

static int mkdir_p(char *dir, char *err, size_t err_n)
{
    char *p;

    if (!dir[0]) {
        return 0;
    }
    for (p = dir + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            set_err(err, err_n, "cannot create directory %s: %s", dir, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        set_err(err, err_n, "cannot create directory %s: %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

static int ensure_parent(const char *path, char *err, size_t err_n)
{
    char dir[AUTODJ_PATH];
    char *slash;

    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) {
        set_err(err, err_n, "config path is too long");
        return -1;
    }
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return 0;
    }
    *slash = '\0';
    return mkdir_p(dir, err, err_n);
}

/* Rewrite [Main] and [Keys], or only [Jog] when jog is set. */
static int save_sections(const char *path, const Config *cfg, int jog, char *err, size_t err_n)
{
    static const char *main_names[] = {
        "Folder", "shuffle", "overlap", "trim_silence", "pause_time",
        "resume_time", "volume", "loudness", "loudness_lufs", "dial_default"
    };
    static const char *key_names[] = {
        "pause_resume", "skip", "previous", "rewind_0_5", "rewind_1",
        "rewind_1_5", "dial_forward", "dial_back", "dial_mode",
        "reverb", "chipmunk", "autopan", "phaser"
    };
    FILE *in = NULL;
    FILE *out = NULL;
    struct CfgLine *head = NULL;
    struct CfgLine *tail = NULL;
    struct CfgLine *line;
    struct CfgLine *prev;
    struct CfgLine *main_header = NULL;
    struct CfgLine *main_last = NULL;
    struct CfgLine *keys_header = NULL;
    struct CfgLine *keys_last = NULL;
    struct CfgLine *jog_header = NULL;
    struct CfgLine *jog_last = NULL;
    char buf[AUTODJ_PATH + 128];
    char tmp[AUTODJ_PATH + 16];
    char section[64];
    char value[AUTODJ_PATH];
    struct stat st;
    int fd = -1;
    int created = 0;
    int in_main = 0;
    int in_keys = 0;
    int in_jog = 0;
    int in_fx = -1;
    int in_old_fx = 0;
    int jog_seen[JOG_COUNT] = {0};
    int main_seen[10] = {0};
    int keys_seen[KEYID_COUNT] = {0};
    int fx_seen[4][7] = {{0}};
    int id;
    int rc = -1;

    if (err && err_n) {
        err[0] = '\0';
    }
    if (!path || !cfg) {
        set_err(err, err_n, "config path is missing");
        return -1;
    }
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) {
        set_err(err, err_n, "config path is too long");
        return -1;
    }
    st.st_mode = 0644;
    in = fopen(path, "r");
    if (!in) {
        if (errno != ENOENT) {
            set_err(err, err_n, "cannot open %s: %s", path, strerror(errno));
            return -1;
        }
    } else {
        if (fstat(fileno(in), &st) != 0) {
            st.st_mode = 0644;
        }
        while (fgets(buf, sizeof buf, in)) {
            if (append_line(&head, &tail, buf) != 0) {
                set_err(err, err_n, "out of memory reading %s", path);
                goto done;
            }
        }
        if (ferror(in)) {
            set_err(err, err_n, "cannot read %s: %s", path, strerror(errno));
            goto done;
        }
    }

    section[0] = '\0';
    prev = NULL;
    line = head;
    while (line) {
        struct CfgLine *next = line->next;
        char key[128];

        if (section_name(line->text, section, sizeof section)) {
            in_main = eq_ci(section, "Main");
            in_keys = eq_ci(section, "Keys");
            in_jog = eq_ci(section, "Jog");
            in_fx = fx_section(section);
            in_old_fx = 0;
            /* Pitch Shift is written again as Chipmunk, or dropped if both are there. */
            if (!jog && in_fx == 1 && !eq_ci(section, "Chipmunk")) {
                struct CfgLine *ck_header;
                struct CfgLine *ck_last;
                char *repl;

                find_section(head, "Chipmunk", &ck_header, &ck_last);
                if (ck_header) {
                    in_old_fx = 1;
                } else {
                    repl = strdup("[Chipmunk]\n");
                    if (!repl) {
                        set_err(err, err_n, "out of memory updating %s", path);
                        goto done;
                    }
                    free(line->text);
                    line->text = repl;
                }
            }
            if (!jog && in_old_fx) {
                unlink_line(&head, prev, line);
                line = next;
                continue;
            }
            prev = line;
            line = next;
            continue;
        }
        if (!jog && in_old_fx && line_setting_key(line->text, key, sizeof key)) {
            unlink_line(&head, prev, line);
            line = next;
            continue;
        }
        if (jog && in_jog && line_setting_key(line->text, key, sizeof key) &&
            (id = jog_id(key)) >= 0) {
            jog_seen[id] = 1;
            jog_value(cfg, id, value, sizeof value);
            if (rewrite_value(line, value) != 0) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
        }
        if (!jog && in_keys && line_setting_key(line->text, key, sizeof key) &&
            (eq_ci(key, "volume_up") || eq_ci(key, "volume_dn") ||
             eq_ci(key, "volume_down") || eq_ci(key, "brake_delayvolume_up"))) {
            unlink_line(&head, prev, line);
            line = next;
            continue;
        }
        /* Effects always start off, so the old startup switch goes away, and so
         * do the settings of the old Mixxx-style phaser. */
        if (!jog && in_fx >= 0 && line_setting_key(line->text, key, sizeof key) &&
            (eq_ci(key, "on") || (in_fx == 3 && old_phaser_key(key)))) {
            unlink_line(&head, prev, line);
            line = next;
            continue;
        }
        if (!jog && in_fx >= 0 && line_setting_key(line->text, key, sizeof key) &&
            (id = fx_key_id(in_fx, key)) >= 0) {
            fx_seen[in_fx][id] = 1;
            fx_value(cfg, in_fx, id, value, sizeof value);
            if (rewrite_value(line, value) != 0) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
        }
        if (!jog && (in_main || in_keys) && line_setting_key(line->text, key, sizeof key)) {
            int is_main = in_main && (id = main_id(key)) >= 0;
            int is_key = in_keys && (id = keys_id(key)) >= 0;

            /* The old names are written again under the new ones. */
            if (in_keys && (eq_ci(key, "jog_forward") || eq_ci(key, "jog_back") ||
                            eq_ci(key, "pitch_shift"))) {
                unlink_line(&head, prev, line);
                line = next;
                continue;
            }
            if (is_main) {
                main_seen[id] = 1;
                main_value(cfg, id, value, sizeof value);
                if (rewrite_value(line, value) != 0) {
                    set_err(err, err_n, "out of memory updating %s", path);
                    goto done;
                }
            } else if (is_key) {
                keys_seen[id] = 1;
                key_value(cfg, id, value, sizeof value);
                if (!value[0]) {
                    unlink_line(&head, prev, line);
                    line = next;
                    continue;
                }
                if (rewrite_value(line, value) != 0) {
                    set_err(err, err_n, "out of memory updating %s", path);
                    goto done;
                }
            }
        }
        prev = line;
        line = next;
    }

    if (!jog && ensure_section(&head, "Main", &main_header, &main_last) != 0) {
        set_err(err, err_n, "out of memory updating %s", path);
        goto done;
    }
    for (id = 0; id < 10 && !jog; id++) {
        char text[AUTODJ_PATH + 64];

        if (main_seen[id]) {
            continue;
        }
        main_value(cfg, id, value, sizeof value);
        if (snprintf(text, sizeof text, "%s=%s\n", main_names[id], value) >= (int)sizeof text) {
            set_err(err, err_n, "a config value is too long");
            goto done;
        }
        main_last = insert_after(&head, main_last, text);
        if (!main_last) {
            set_err(err, err_n, "out of memory updating %s", path);
            goto done;
        }
    }

    for (id = 0; id < KEYID_COUNT && !jog; id++) {
        key_value(cfg, id, value, sizeof value);
        if (!keys_seen[id] && value[0]) {
            break;
        }
    }
    if (!jog && id < KEYID_COUNT) {
        if (ensure_section(&head, "Keys", &keys_header, &keys_last) != 0) {
            set_err(err, err_n, "out of memory updating %s", path);
            goto done;
        }
        for (id = 0; id < KEYID_COUNT; id++) {
            char text[160];

            if (keys_seen[id]) {
                continue;
            }
            key_value(cfg, id, value, sizeof value);
            if (!value[0]) {
                continue;
            }
            if (snprintf(text, sizeof text, "%s=%s\n", key_names[id], value) >= (int)sizeof text) {
                set_err(err, err_n, "a config value is too long");
                goto done;
            }
            keys_last = insert_after(&head, keys_last, text);
            if (!keys_last) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
        }
    }

    if (!jog) {
        static const char *fx_names[] = {"Reverb", "Chipmunk", "Autopan", "Phaser"};
        static const int fx_count[] = {4, 4, 3, 7};
        int fx;

        for (fx = 0; fx < 4; fx++) {
            struct CfgLine *fx_header = NULL;
            struct CfgLine *fx_last = NULL;

            if (ensure_section(&head, fx_names[fx], &fx_header, &fx_last) != 0) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
            for (id = 0; id < fx_count[fx]; id++) {
                char text[80];
                char fxv[32];

                if (fx_seen[fx][id]) {
                    continue;
                }
                fx_value(cfg, fx, id, fxv, sizeof fxv);
                snprintf(text, sizeof text, "%s=%s\n", fx_key_name(fx, id), fxv);
                fx_last = insert_after(&head, fx_last, text);
                if (!fx_last) {
                    set_err(err, err_n, "out of memory updating %s", path);
                    goto done;
                }
            }
        }
    }

    if (jog) {
        if (ensure_section(&head, "Jog", &jog_header, &jog_last) != 0) {
            set_err(err, err_n, "out of memory updating %s", path);
            goto done;
        }
        for (id = 0; id < JOG_COUNT; id++) {
            char text[160];

            if (jog_seen[id]) {
                continue;
            }
            jog_value(cfg, id, value, sizeof value);
            snprintf(text, sizeof text, "%s=%.60s\n", g_jog_names[id], value);
            jog_last = insert_after(&head, jog_last, text);
            if (!jog_last) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
        }
    }

    if (ensure_parent(path, err, err_n) != 0) {
        goto done;
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        set_err(err, err_n, "cannot update %s: %s", path, strerror(errno));
        goto done;
    }
    created = 1;
    if (fchmod(fd, st.st_mode & 0777) != 0) {
        log_warn("autodj", "could not keep permissions on %s: %s", path, strerror(errno));
    }
    out = fdopen(fd, "w");
    if (!out) {
        set_err(err, err_n, "cannot update %s: %s", path, strerror(errno));
        goto done;
    }
    fd = -1;
    for (line = head; line; line = line->next) {
        fputs(line->text, out);
    }
    if (ferror(out) || fflush(out) != 0 || fsync(fileno(out)) != 0) {
        set_err(err, err_n, "cannot write %s: %s", path, strerror(errno));
        goto done;
    }
    fclose(out);
    out = NULL;
    if (in) {
        fclose(in);
        in = NULL;
    }
    if (rename(tmp, path) != 0) {
        set_err(err, err_n, "cannot replace %s: %s", path, strerror(errno));
        goto done;
    }
    created = 0;
    rc = 0;

done:
    if (out) {
        fclose(out);
    } else if (fd >= 0) {
        close(fd);
    }
    if (in) {
        fclose(in);
    }
    if (created) {
        unlink(tmp);
    }
    free_lines(head);
    return rc;
}

static int file_count(const char *path, const char *needle)
{
    FILE *fp;
    char buf[8192];
    size_t n;
    size_t nlen;
    int count = 0;
    char *p;

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    nlen = strlen(needle);
    p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

static int read_text(const char *path, char *buf, size_t n)
{
    FILE *fp;
    size_t got;

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    got = fread(buf, 1, n - 1, fp);
    buf[got] = '\0';
    if (ferror(fp) || got == n - 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int edit_self_test(void)
{
    char path[] = "/tmp/autodj-edit-XXXXXX";
    char dir[] = "/tmp/autodj-cfgdir-XXXXXX";
    char nested[256];
    char sub[256];
    char got[4096];
    const char *input =
        "# keep me\n"
        "[Main]\n"
        "Folder=/tmp\n"
        "volume = 40\n"
        "\n"
        "[Keys]\n"
        "# key comment\n"
        "skip=KEY_KPDOT\n"
        "volume_down=KEY_KPMINUS\n"
        "custom=KEY_A\n"
        "phaser=KEY_B\n"
        "pitch_shift=KEY_C\n"
        "[Reverb]\n"
        "on=true\n"
        "[Pitch Shift]\n"
        "pitch=0.5\n"
        "[Phaser]\n"
        "on=true\n"
        "depth=0.7\n"
        "stages=7\n"
        "stereo=true\n";
    const char *expect =
        "# keep me\n"
        "[Main]\n"
        "Folder=/tmp\n"
        "volume = 40\n"
        "shuffle=true\n"
        "overlap=8\n"
        "trim_silence=true\n"
        "pause_time=3\n"
        "resume_time=4\n"
        "loudness=true\n"
        "loudness_lufs=-18\n"
        "dial_default=volume\n"
        "\n"
        "[Keys]\n"
        "# key comment\n"
        "skip=KEY_KPDOT\n"
        "custom=KEY_A\n"
        "phaser=KEY_B\n"
        "pause_resume=KEY_KP0\n"
        "chipmunk=KEY_C\n"
        "[Reverb]\n"
        "decay=0.5\n"
        "bw=1\n"
        "damping=0\n"
        "send=0.5\n"
        "[Chipmunk]\n"
        "pitch=1\n"
        "range=1\n"
        "semitones=true\n"
        "formant=false\n"
        "[Phaser]\n"
        "stages=7\n"
        "period=1\n"
        "low=300\n"
        "high=3000\n"
        "feedback=0.5\n"
        "mix=50\n"
        "boost=15\n"
        "[Autopan]\n"
        "period=4\n"
        "smoothing=0.5\n"
        "distance=75\n";
    Config cfg;
    ConfigEdit edit;
    char err[512];
    int fd;
    FILE *fp;
    int rc = 0;

    fd = mkstemp(path);
    if (fd < 0) {
        log_error("autodj", "config editor test could not create a temp file");
        return -1;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        log_error("autodj", "config editor test could not write a temp file");
        close(fd);
        remove(path);
        return -1;
    }
    fputs(input, fp);
    fclose(fp);

    if (config_load_edit(path, &edit) != 0 ||
        strcmp(edit.folder, "/tmp") != 0 || strcmp(edit.volume, "40") != 0 ||
        strcmp(edit.shuffle, "true") != 0 || strcmp(edit.key_skip, "KEY_KPDOT") != 0 ||
        edit.key_reverb[0] != '\0' || edit.key_pause[0] != '\0' ||
        strcmp(edit.rv_decay, "0.5") != 0 || strcmp(edit.ap_distance, "75") != 0 ||
        edit.key_autopan[0] != '\0' || strcmp(edit.key_phaser, "KEY_B") != 0 ||
        strcmp(edit.ps_semi, "true") != 0 || strcmp(edit.ph_stages, "7") != 0 ||
        strcmp(edit.ph_low, "300") != 0 || strcmp(edit.ph_mix, "50") != 0 ||
        strcmp(edit.key_pitch, "KEY_C") != 0 || strcmp(edit.ps_pitch, "0.5") != 0) {
        log_error("autodj", "config editor test loaded unexpected text");
        rc = -1;
    }
    memset(&cfg, 0, sizeof cfg);
    fx_defaults(&cfg.fx);
    snprintf(cfg.folder, sizeof cfg.folder, "/tmp");
    cfg.shuffle = 1;
    cfg.overlap_sec = 8;
    cfg.trim_silence = 1;
    cfg.pause_sec = 3;
    cfg.resume_sec = 4;
    cfg.volume = 40;
    cfg.loudness = 1;
    cfg.loudness_lufs = -18;
    cfg.dial_volume = 1;
    snprintf(cfg.key_pause_name, sizeof cfg.key_pause_name, "KEY_KP0");
    snprintf(cfg.key_skip_name, sizeof cfg.key_skip_name, "KEY_KPDOT");
    snprintf(cfg.key_pitch_name, sizeof cfg.key_pitch_name, "KEY_C");
    snprintf(cfg.key_phaser_name, sizeof cfg.key_phaser_name, "KEY_B");
    cfg.fx.ph_stages = 7;
    if (rc == 0 && (config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 || strcmp(got, expect) != 0)) {
        log_error("autodj", "config editor test did not keep the file layout: %s\n%s", err, got);
        rc = -1;
    }
    cfg.key_skip_name[0] = '\0';
    if (rc == 0 && (config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 ||
                    strstr(got, "volume_down=") != NULL ||
                    strstr(got, "skip=") != NULL ||
                    strstr(got, "pause_resume=KEY_KP0\n") == NULL ||
                    strstr(got, "phaser=KEY_B\n") == NULL ||
                    strstr(got, "autopan=") != NULL ||
                    strstr(got, "custom=KEY_A\n") == NULL)) {
        log_error("autodj", "config editor test did not clear a key\n%s", got);
        rc = -1;
    }
    /* With both section names present, the old one is dropped. */
    fp = rc == 0 ? fopen(path, "w") : NULL;
    if (fp) {
        fputs("[Chipmunk]\npitch=0.25\n[Pitch Shift]\npitch=0.5\nrange=2\n", fp);
        fclose(fp);
    }
    if (rc == 0 && (!fp || config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 ||
                    strstr(got, "[Pitch Shift]") != NULL ||
                    strstr(got, "[Chipmunk]\npitch=1\nrange=1\n") == NULL ||
                    file_count(path, "[Chipmunk]") != 1)) {
        log_error("autodj", "config editor test kept two chipmunk sections: %s\n%s", err, got);
        rc = -1;
    }
    remove(path);

    if (rc == 0 && (config_load_edit("/tmp/autodj-no-such-config.cfg", &edit) != 0 ||
                    edit.folder[0] != '\0' || strcmp(edit.overlap, "10") != 0 ||
                    strcmp(edit.loudness_lufs, "-18") != 0 || edit.key_skip[0] != '\0' ||
                    edit.key_pause[0] != '\0' ||
                    strcmp(edit.key_prev, "KEY_KPSLASH") != 0 ||
                    strcmp(edit.key_rew05, "KEY_KP1") != 0 ||
                    strcmp(edit.key_rew10, "KEY_KP2") != 0 ||
                    strcmp(edit.key_rew15, "KEY_KP3") != 0 ||
                    strcmp(edit.rv_decay, "0.5") != 0 ||
                    strcmp(edit.ps_semi, "true") != 0 ||
                    strcmp(edit.ap_period, "4") != 0 ||
                    strcmp(edit.ap_smooth, "0.5") != 0 ||
                    edit.key_reverb[0] != '\0')) {
        log_error("autodj", "config editor test rejected a missing file");
        rc = -1;
    }

    memset(&edit, 0, sizeof edit);
    snprintf(edit.folder, sizeof edit.folder, "/tmp");
    snprintf(edit.shuffle, sizeof edit.shuffle, "true");
    snprintf(edit.overlap, sizeof edit.overlap, "8");
    snprintf(edit.trim_silence, sizeof edit.trim_silence, "true");
    snprintf(edit.pause_time, sizeof edit.pause_time, "3");
    snprintf(edit.resume_time, sizeof edit.resume_time, "4");
    snprintf(edit.volume, sizeof edit.volume, "40");
    snprintf(edit.loudness, sizeof edit.loudness, "true");
    snprintf(edit.loudness_lufs, sizeof edit.loudness_lufs, "-18");
    snprintf(edit.dial_default, sizeof edit.dial_default, "volume");
    snprintf(edit.key_pause, sizeof edit.key_pause, "KEY_KP0");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KPDOT");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) != 0 ||
                    strcmp(cfg.folder, "/tmp") != 0 || cfg.volume != 40 ||
                    cfg.key_pause != 82 || cfg.key_reverb != -1 ||
                    cfg.key_pitch != -1 || cfg.key_autopan != -1 ||
                    cfg.key_prev != -1 || cfg.key_rew05 != -1 ||
                    cfg.key_rew10 != -1 || cfg.key_rew15 != -1 ||
                    cfg.fx.rv_decay != 0.5f || cfg.fx.ps_semi != 1 ||
                    cfg.fx.ap_period != 4 || cfg.fx.ap_distance != 75 ||
                    cfg.fx.ph_low != 300 || cfg.fx.ph_stages != 6)) {
        log_error("autodj", "config editor test rejected a valid form: %s", err);
        rc = -1;
    }
    snprintf(edit.ph_low, sizeof edit.ph_low, "3000");
    snprintf(edit.ph_high, sizeof edit.ph_high, "3000");
    snprintf(edit.ph_stages, sizeof edit.ph_stages, "2.5");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
                    strstr(err, "Low Hz must be below High Hz.") == NULL ||
                    strstr(err, "Stages must be a whole number from 1 to 12.") == NULL)) {
        log_error("autodj", "config editor test missed a bad phaser: %s", err);
        rc = -1;
    }
    edit.ph_low[0] = '\0';
    edit.ph_high[0] = '\0';
    edit.ph_stages[0] = '\0';
    snprintf(edit.volume, sizeof edit.volume, "150");
    snprintf(edit.folder, sizeof edit.folder, "/tmp/autodj-missing-folder");
    snprintf(edit.shuffle, sizeof edit.shuffle, "maybe");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KP0");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
                    strstr(err, "Folder does not exist.") == NULL ||
                    strstr(err, "Shuffle must be true or false.") == NULL ||
                    strstr(err, "Volume must be a whole number from 0 to 100.") == NULL ||
                    strstr(err, "KEY_KP0 is used for pause/resume and skip.") == NULL)) {
        log_error("autodj", "config editor test missed a bad form: %s", err);
        rc = -1;
    }

    memset(&edit, 0, sizeof edit);
    snprintf(edit.folder, sizeof edit.folder, "/tmp");
    snprintf(edit.shuffle, sizeof edit.shuffle, "true");
    snprintf(edit.overlap, sizeof edit.overlap, "8");
    snprintf(edit.trim_silence, sizeof edit.trim_silence, "true");
    snprintf(edit.pause_time, sizeof edit.pause_time, "3");
    snprintf(edit.resume_time, sizeof edit.resume_time, "4");
    snprintf(edit.volume, sizeof edit.volume, "40");
    snprintf(edit.loudness, sizeof edit.loudness, "true");
    snprintf(edit.loudness_lufs, sizeof edit.loudness_lufs, "-18");
    snprintf(edit.key_pause, sizeof edit.key_pause, "KEY_KP0");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KPDOT");
    snprintf(edit.key_prev, sizeof edit.key_prev, "KEY_KPSLASH");
    snprintf(edit.key_rew05, sizeof edit.key_rew05, "KEY_KP1");
    snprintf(edit.key_rew10, sizeof edit.key_rew10, "KEY_KP1");
    snprintf(edit.key_rew15, sizeof edit.key_rew15, "KEY_KP3");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
                    strstr(err, "KEY_KP1 is used for rewind 0.5s and rewind 1s.") == NULL)) {
        log_error("autodj", "config editor test missed a rewind conflict: %s", err);
        rc = -1;
    }
    snprintf(edit.key_rew10, sizeof edit.key_rew10, "KEY_KP2");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) != 0 ||
                    cfg.key_pause != 82 || cfg.key_prev < 0 || cfg.key_rew05 < 0 ||
                    cfg.key_rew05 == cfg.key_rew10 || cfg.key_rew15 < 0)) {
        log_error("autodj", "config editor test rejected separate rewind keys: %s", err);
        rc = -1;
    }

    if (!mkdtemp(dir)) {
        log_error("autodj", "config editor test could not create a temp directory");
        return -1;
    }
    snprintf(nested, sizeof nested, "%s/sub/master.cfg", dir);
    snprintf(sub, sizeof sub, "%s/sub", dir);
    memset(&cfg, 0, sizeof cfg);
    fx_defaults(&cfg.fx);
    snprintf(cfg.folder, sizeof cfg.folder, "/tmp");
    cfg.shuffle = 1;
    cfg.overlap_sec = 8;
    cfg.trim_silence = 1;
    cfg.pause_sec = 3;
    cfg.resume_sec = 4;
    cfg.volume = 12;
    cfg.loudness = 1;
    cfg.loudness_lufs = -18;
    if (rc == 0 && (config_save(nested, &cfg, err, sizeof err) != 0 ||
                    config_load(nested, &cfg) != 0 || cfg.volume != 12 ||
                    cfg.overlap_sec != 8 || strcmp(cfg.folder, "/tmp") != 0)) {
        log_error("autodj", "config editor test did not create a new config: %s", err);
        rc = -1;
    }
    remove(nested);
    rmdir(sub);
    rmdir(dir);
    return rc;
}

int config_save(const char *path, const Config *cfg, char *err, size_t err_n)
{
    return save_sections(path, cfg, 0, err, err_n);
}

int config_save_jog(const char *path, const Config *cfg, char *err, size_t err_n)
{
    return save_sections(path, cfg, 1, err, err_n);
}

int config_self_test(void)
{
    char path[] = "/tmp/autodj-cfg-XXXXXX";
    int fd;
    FILE *fp;
    Config cfg;
    int slash = -1;
    int kp1 = -1;
    int kp2 = -1;
    int kp3 = -1;
    int key_a = -1;
    int key_b = -1;
    int rc = 0;

    if (key_from_name("KEY_KPSLASH", &slash) != 0 || key_from_name("KEY_KP1", &kp1) != 0 ||
        key_from_name("KEY_KP2", &kp2) != 0 || key_from_name("KEY_KP3", &kp3) != 0 ||
        key_from_name("KEY_A", &key_a) != 0 || key_from_name("KEY_B", &key_b) != 0) {
        log_error("autodj", "config test could not resolve default key names");
        return -1;
    }

    fd = mkstemp(path);
    if (fd < 0) {
        log_error("autodj", "config test could not create a temp file");
        return -1;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        log_error("autodj", "config test could not write a temp file");
        return -1;
    }
    fputs("[Main]\n"
          "Folder=/tmp/music folder\n"
          "shuffle=false\n"
          "overlap=3\n"
          "trim_silence=true\n"
          "pause_time=2\n"
          "resume_time=4\n"
          "\n"
          "[Keys]\n"
          "pause_resume=KEY_KP0\n"
          "skip=KEY_KPDOT\n"
          "volume_up=KEY_KPPLUS\n"
          "volume_dn=KEY_KPMINUS\n"
          "phaser=KEY_A\n"
          "pitch_shift=KEY_B\n"
          "[Reverb]\n"
          "on=true\n"
          "[Pitch Shift]\n"
          "pitch=-0.5\n"
          "[Autopan]\n"
          "on=true\n"
          "period=1.5\n"
          "smoothing=0.3\n"
          "[Phaser]\n"
          "depth=0.7\n"
          "stages=7\n"
          "triplets=true\n"
          "mix=80\n"
          "low=4000\n",
          fp);
    fclose(fp);

    if (config_load(path, &cfg) != 0) {
        rc = -1;
    } else if (strcmp(cfg.folder, "/tmp/music folder") != 0 || cfg.shuffle != 0 ||
               cfg.overlap_sec != 3 || cfg.trim_silence != 1 || cfg.pause_sec != 2 ||
               cfg.resume_sec != 4 || cfg.volume != 100 || cfg.volume_set != 0 ||
               cfg.loudness != 1 || cfg.loudness_lufs != -18 || cfg.dial_volume != 1 ||
               cfg.key_pause != 82 || cfg.key_skip != 83 ||
               cfg.key_reverb != -1 || cfg.key_pitch != key_b || cfg.key_autopan != -1 ||
               cfg.key_phaser != key_a || cfg.fx.phaser_on != 0 ||
               cfg.fx.ps_pitch != -0.5f ||
               cfg.fx.rv_decay != 0.5f || cfg.fx.ps_semi != 1 || cfg.fx.ap_period != 1.5f ||
               cfg.fx.ap_smooth != 0.3f || cfg.fx.ap_distance != 75 ||
               cfg.fx.ph_stages != 7 || cfg.fx.ph_blend != 80 || cfg.fx.ph_low != 300 ||
               cfg.fx.ph_high != 3000 || cfg.fx.ph_period != 1 ||
               cfg.fx.reverb_on != 0 || cfg.fx.pitch_on != 0 || cfg.fx.autopan_on != 0 ||
               cfg.key_prev != slash || cfg.key_rew05 != kp1 ||
               cfg.key_rew10 != kp2 || cfg.key_rew15 != kp3) {
        log_error("autodj", "config test parsed unexpected values");
        rc = -1;
    } else if (config_save_volume(path, 15) != 0 ||
               file_count(path, "volume=") != 1 ||
               file_count(path, "volume=15\n") != 1 ||
               file_count(path, "volume_up=KEY_KPPLUS\n") != 1 ||
               file_count(path, "Folder=/tmp/music folder\n") != 1) {
        log_error("autodj", "config test did not store volume");
        rc = -1;
    } else if (config_load(path, &cfg) != 0 || cfg.volume != 15 || cfg.volume_set != 1 ||
               strcmp(cfg.folder, "/tmp/music folder") != 0 || cfg.key_reverb != -1) {
        log_error("autodj", "config test did not reload stored volume");
        rc = -1;
    } else if (config_save_volume(path, 0) != 0 || file_count(path, "volume=") != 1 ||
               config_load(path, &cfg) != 0 || cfg.volume != 0) {
        log_error("autodj", "config test did not store a zero volume");
        rc = -1;
    } else if (file_count(path, "[Phaser]\ndepth=0.7\n") != 1 ||
               file_count(path, "volume_up=KEY_KPPLUS\n") != 1 ||
               cfg.fx.reverb_on != 0 || cfg.fx.autopan_on != 0) {
        log_error("autodj", "config test changed other lines or turned an effect on");
        rc = -1;
    }
    remove(path);
    if (rc != 0) {
        return rc;
    }

    {
        char path2[] = "/tmp/autodj-cfg-XXXXXX";

        fd = mkstemp(path2);
        if (fd < 0) {
            log_error("autodj", "config test could not create a temp file");
            return -1;
        }
        fp = fdopen(fd, "w");
        if (!fp) {
            log_error("autodj", "config test could not write a temp file");
            close(fd);
            remove(path2);
            return -1;
        }
        fputs("[Main]\n"
              "Folder=/tmp/music folder\n"
              "volume = 40\n",
              fp);
        fclose(fp);
        if (config_load(path2, &cfg) != 0 || cfg.volume != 40 || cfg.volume_set != 1) {
            log_error("autodj", "config test rejected a valid volume");
            rc = -1;
        } else if (config_save_volume(path2, 7) != 0 ||
                   file_count(path2, "volume = 7\n") != 1 ||
                   file_count(path2, "Folder=/tmp/music folder\n") != 1) {
            log_error("autodj", "config test rewrote volume lines incorrectly");
            rc = -1;
        }
        remove(path2);
    }
    if (rc == 0) {
        char path3[] = "/tmp/autodj-cfg-XXXXXX";

        fd = mkstemp(path3);
        if (fd < 0) {
            log_error("autodj", "config test could not create a temp file");
            return -1;
        }
        fp = fdopen(fd, "w");
        if (!fp) {
            log_error("autodj", "config test could not write a temp file");
            close(fd);
            remove(path3);
            return -1;
        }
        fputs("[Main]\n"
              "Folder=/tmp\n"
              "[Keys]\n"
              "skip=KEY_KPSLASH\n"
              "previous=KEY_A\n",
              fp);
        fclose(fp);
        if (config_load(path3, &cfg) != 0 || cfg.key_skip != slash ||
            cfg.key_prev != key_a || cfg.key_rew05 != kp1 ||
            cfg.key_rew10 != kp2 || cfg.key_rew15 != kp3) {
            log_error("autodj", "config test did not keep an explicit previous key");
            rc = -1;
        }
        fp = fopen(path3, "w");
        if (!fp) {
            log_error("autodj", "config test could not rewrite a temp file");
            remove(path3);
            return -1;
        }
        fputs("[Main]\n"
              "Folder=/tmp\n"
              "[Keys]\n"
              "skip=KEY_KPSLASH\n",
              fp);
        fclose(fp);
        if (rc == 0 && (config_load(path3, &cfg) != 0 || cfg.key_skip != slash ||
                        cfg.key_prev != -1 || cfg.key_rew05 != kp1 ||
                        cfg.key_rew10 != kp2 || cfg.key_rew15 != kp3)) {
            log_error("autodj", "config test applied a previous key that was already used");
            rc = -1;
        }
        remove(path3);
    }
    if (rc == 0) {
        char path4[] = "/tmp/autodj-cfg-XXXXXX";

        fd = mkstemp(path4);
        if (fd < 0) {
            log_error("autodj", "config test could not create a temp file");
            return -1;
        }
        fp = fdopen(fd, "w");
        if (!fp) {
            log_error("autodj", "config test could not write a temp file");
            close(fd);
            remove(path4);
            return -1;
        }
        fputs("[Main]\n"
              "Folder=/tmp\n"
              "[Jog]\n"
              "clicks_per_turn = 24\n"
              "max_speed=99\n"
              "[Other]\n"
              "x=1\n",
              fp);
        fclose(fp);
        if (config_load(path4, &cfg) != 0 || cfg.jog_clicks != 24 || cfg.jog_max_speed != 4.0 ||
            cfg.jog_turn_sec != 1.8 || cfg.jog_smooth_sec != 0.01) {
            log_error("autodj", "config test parsed unexpected jog values");
            rc = -1;
        } else {
            char err[256];

            cfg.jog_clicks = 30;
            cfg.jog_spinup_sec = 0;
            if (config_save_jog(path4, &cfg, err, sizeof err) != 0 ||
                file_count(path4, "shuffle") != 0 ||
                file_count(path4, "clicks_per_turn = 30\n") != 1 ||
                file_count(path4, "max_speed=4\n") != 1 ||
                file_count(path4, "spinup_time=0\n") != 1 ||
                file_count(path4, "[Jog]") != 1 || file_count(path4, "x=1\n") != 1 ||
                config_load(path4, &cfg) != 0 || cfg.jog_clicks != 30 ||
                cfg.jog_spinup_sec != 0) {
                log_error("autodj", "config test saved jog values incorrectly");
                rc = -1;
            }
        }
        remove(path4);
    }
    if (rc == 0) {
        char path5[] = "/tmp/autodj-cfg-XXXXXX";
        char err[256];
        int cw = -1;
        int ccw = -1;
        int center = -1;

        fd = mkstemp(path5);
        if (fd < 0) {
            log_error("autodj", "config test could not create a temp file");
            return -1;
        }
        fp = fdopen(fd, "w");
        if (!fp) {
            log_error("autodj", "config test could not write a temp file");
            close(fd);
            remove(path5);
            return -1;
        }
        fputs("[Main]\n"
              "Folder=/tmp\n"
              "dial_default=JOG\n"
              "[Keys]\n"
              "jog_forward=REMOTE_DIAL_CW\n"
              "jog_back=REMOTE_DIAL_CCW\n"
              "dial_mode=REMOTE_CENTER\n",
              fp);
        fclose(fp);
        if (key_from_name("REMOTE_DIAL_CW", &cw) != 0 || key_from_name("REMOTE_DIAL_CCW", &ccw) != 0 ||
            key_from_name("REMOTE_CENTER", &center) != 0 || config_load(path5, &cfg) != 0 ||
            cfg.key_dial_fwd != cw || cfg.key_dial_back != ccw || cfg.key_dial_mode != center ||
            cfg.dial_volume != 0) {
            log_error("autodj", "config test did not read the dial keys");
            rc = -1;
        } else if (config_save(path5, &cfg, err, sizeof err) != 0 ||
                   file_count(path5, "jog_") != 0 ||
                   file_count(path5, "dial_forward=REMOTE_DIAL_CW\n") != 1 ||
                   file_count(path5, "dial_back=REMOTE_DIAL_CCW\n") != 1 ||
                   file_count(path5, "dial_mode=REMOTE_CENTER\n") != 1 ||
                   file_count(path5, "dial_default=jog\n") != 1 ||
                   config_load(path5, &cfg) != 0 || cfg.key_dial_fwd != cw) {
            log_error("autodj", "config test did not rename the jog keys");
            rc = -1;
        }
        remove(path5);
    }
    if (rc == 0 && edit_self_test() != 0) {
        rc = -1;
    }
    return rc;
}
