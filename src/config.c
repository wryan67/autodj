#include "config.h"

#include "keys.h"
#include "log.h"
#include "midi.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* The file named this action and left the value empty. A missing keypad
 * line stays unbound. An empty one must not become a key. */
enum { KEY_EXPLICIT_UNBOUND = -2 };

static int keys_id(const char *key);

/* Copy action without suffix (such as "_kbd") into base. Returns 1 when
 * action ends with it. */
static int strip_suffix(const char *action, const char *suffix, char *base, size_t n)
{
    size_t len;
    size_t slen = strlen(suffix);

    if (!action) {
        return 0;
    }
    len = strlen(action);
    if (len <= slen || len - slen >= n || strcasecmp(action + len - slen, suffix) != 0) {
        return 0;
    }
    memcpy(base, action, len - slen);
    base[len - slen] = '\0';
    return 1;
}

static int strip_kbd(const char *action, char *base, size_t n)
{
    return strip_suffix(action, "_kbd", base, n);
}

static int strip_midi(const char *action, char *base, size_t n)
{
    return strip_suffix(action, "_midi", base, n);
}

static char *kbd_cfg(Config *cfg, int id)
{
    switch (id) {
    case KEYID_PAUSE:
        return cfg->kbd_pause;
    case KEYID_SKIP:
        return cfg->kbd_skip;
    case KEYID_PREV:
        return cfg->kbd_prev;
    case KEYID_REW05:
        return cfg->kbd_rew05;
    case KEYID_REW10:
        return cfg->kbd_rew10;
    case KEYID_REW15:
        return cfg->kbd_rew15;
    case KEYID_DIAL_FWD:
        return cfg->kbd_dial_fwd;
    case KEYID_DIAL_BACK:
        return cfg->kbd_dial_back;
    case KEYID_DIAL_MODE:
        return cfg->kbd_dial_mode;
    case KEYID_REVERB:
        return cfg->kbd_reverb;
    case KEYID_PITCH:
        return cfg->kbd_pitch;
    case KEYID_AUTOPAN:
        return cfg->kbd_autopan;
    case KEYID_PHASER:
        return cfg->kbd_phaser;
    case KEYID_CHORUS:
        return cfg->kbd_chorus;
    case KEYID_FLANGER:
        return cfg->kbd_flanger;
    case KEYID_EFFECTS_OFF:
        return cfg->kbd_effects_off;
    case KEYID_VOL_UP:
        return cfg->kbd_vol_up;
    case KEYID_VOL_DOWN:
        return cfg->kbd_vol_down;
    case KEYID_JOG_UP:
        return cfg->kbd_jog_up;
    case KEYID_JOG_DOWN:
        return cfg->kbd_jog_down;
    default:
        return NULL;
    }
}

static char *kbd_edit(ConfigEdit *edit, int id)
{
    switch (id) {
    case KEYID_PAUSE:
        return edit->kbd_pause;
    case KEYID_SKIP:
        return edit->kbd_skip;
    case KEYID_PREV:
        return edit->kbd_prev;
    case KEYID_REW05:
        return edit->kbd_rew05;
    case KEYID_REW10:
        return edit->kbd_rew10;
    case KEYID_REW15:
        return edit->kbd_rew15;
    case KEYID_DIAL_FWD:
        return edit->kbd_dial_fwd;
    case KEYID_DIAL_BACK:
        return edit->kbd_dial_back;
    case KEYID_DIAL_MODE:
        return edit->kbd_dial_mode;
    case KEYID_REVERB:
        return edit->kbd_reverb;
    case KEYID_PITCH:
        return edit->kbd_pitch;
    case KEYID_AUTOPAN:
        return edit->kbd_autopan;
    case KEYID_PHASER:
        return edit->kbd_phaser;
    case KEYID_CHORUS:
        return edit->kbd_chorus;
    case KEYID_FLANGER:
        return edit->kbd_flanger;
    case KEYID_EFFECTS_OFF:
        return edit->kbd_effects_off;
    case KEYID_VOL_UP:
        return edit->kbd_vol_up;
    case KEYID_VOL_DOWN:
        return edit->kbd_vol_down;
    case KEYID_JOG_UP:
        return edit->kbd_jog_up;
    case KEYID_JOG_DOWN:
        return edit->kbd_jog_down;
    default:
        return NULL;
    }
}

static char *key_edit(ConfigEdit *edit, int id)
{
    switch (id) {
    case KEYID_PAUSE:
        return edit->key_pause;
    case KEYID_SKIP:
        return edit->key_skip;
    case KEYID_PREV:
        return edit->key_prev;
    case KEYID_REW05:
        return edit->key_rew05;
    case KEYID_REW10:
        return edit->key_rew10;
    case KEYID_REW15:
        return edit->key_rew15;
    case KEYID_DIAL_FWD:
        return edit->key_dial_fwd;
    case KEYID_DIAL_BACK:
        return edit->key_dial_back;
    case KEYID_DIAL_MODE:
        return edit->key_dial_mode;
    case KEYID_REVERB:
        return edit->key_reverb;
    case KEYID_PITCH:
        return edit->key_pitch;
    case KEYID_AUTOPAN:
        return edit->key_autopan;
    case KEYID_PHASER:
        return edit->key_phaser;
    case KEYID_CHORUS:
        return edit->key_chorus;
    case KEYID_FLANGER:
        return edit->key_flanger;
    case KEYID_EFFECTS_OFF:
        return edit->key_effects_off;
    case KEYID_VOL_UP:
        return edit->key_vol_up;
    case KEYID_VOL_DOWN:
        return edit->key_vol_down;
    case KEYID_JOG_UP:
        return edit->key_jog_up;
    case KEYID_JOG_DOWN:
        return edit->key_jog_down;
    default:
        return NULL;
    }
}

char *config_edit_binding(ConfigEdit *edit, int id, int column, size_t *cap)
{
    char *text = NULL;
    size_t n = 0;

    if (edit && id >= 0 && id < KEYID_COUNT) {
        if (column == 0) {
            text = kbd_edit(edit, id);
            n = AUTODJ_KBD_LEN;
        } else if (column == 1) {
            text = key_edit(edit, id);
            n = sizeof edit->key_pause;
        } else if (column == 2) {
            text = edit->midi[id];
            n = sizeof edit->midi[id];
        }
    }
    if (cap) {
        *cap = text ? n : 0;
    }
    return text;
}

/* Used when the file does not name that keyboard action. */
static const char *kbd_default(int id)
{
    static const char *vals[KEYID_COUNT] = {
        "Space", "Right", "Left", "1", "2", "3",
        "Up", "Down", "Enter", "q", "w", "e", "r", "t", "y", "Tab",
        "=", "-", ".", ","
    };

    if (id < 0 || id >= KEYID_COUNT) {
        return "";
    }
    return vals[id];
}

static void copy_kbd(char *dest, size_t n, const char *val)
{
    char canon[AUTODJ_KBD_LEN];

    if (!dest || n == 0) {
        return;
    }
    if (!val || !val[0]) {
        dest[0] = '\0';
        return;
    }
    if (keys_kbd_canon(val, canon, sizeof canon) == 0) {
        snprintf(dest, n, "%s", canon);
        return;
    }
    snprintf(dest, n, "%s", val);
}

static void set_key(Config *cfg, const char *action, const char *keyname)
{
    int code = -1;
    int *slot = NULL;
    char *name = NULL;
    size_t name_n = 0;

    if (eq_ci(action, "brake_delayvolume_up") || eq_ci(action, "volume_dn")) {
        /* Old volume keys from before the dial set the volume. */
        return;
    }
    {
        char base[64];
        int id;

        if (strip_midi(action, base, sizeof base) && (id = keys_id(base)) >= 0) {
            char canon[AUTODJ_MIDI_LEN];

            cfg->midi[id][0] = '\0';
            cfg->midi_code[id] = -1;
            if (!keyname[0]) {
                return;
            }
            if (midi_canon(keyname, canon, sizeof canon) != 0 ||
                midi_from_name(canon, &code) != 0) {
                log_warn("autodj", "unknown MIDI control %s for %s", keyname, action);
                return;
            }
            snprintf(cfg->midi[id], AUTODJ_MIDI_LEN, "%s", canon);
            cfg->midi_code[id] = code;
            return;
        }
    }
    {
        char base[64];
        int id;

        if (strip_kbd(action, base, sizeof base) && (id = keys_id(base)) >= 0) {
            char *slot = kbd_cfg(cfg, id);
            char canon[AUTODJ_KBD_LEN];

            if (!slot) {
                return;
            }
            /* A line that is present, even a blank or unknown one, blocks the default. */
            cfg->kbd_line |= 1u << id;
            if (!keyname[0]) {
                slot[0] = '\0';
                return;
            }
            if (keys_kbd_canon(keyname, canon, sizeof canon) != 0) {
                log_warn("autodj", "unknown keyboard key %s for %s", keyname, action);
                return;
            }
            snprintf(slot, AUTODJ_KBD_LEN, "%s", canon);
            return;
        }
    }
    if (eq_ci(action, "pitch_shift") && cfg->key_pitch >= 0) {
        /* A chipmunk line wins over the old pitch_shift name. */
        return;
    }
    if (eq_ci(action, "pause_resume")) {
        slot = &cfg->key_pause;
        name = cfg->key_pause_name;
        name_n = sizeof cfg->key_pause_name;
    } else if (eq_ci(action, "skip")) {
        slot = &cfg->key_skip;
        name = cfg->key_skip_name;
        name_n = sizeof cfg->key_skip_name;
    } else if (eq_ci(action, "previous")) {
        slot = &cfg->key_prev;
        name = cfg->key_prev_name;
        name_n = sizeof cfg->key_prev_name;
    } else if (eq_ci(action, "rewind_0_5")) {
        slot = &cfg->key_rew05;
        name = cfg->key_rew05_name;
        name_n = sizeof cfg->key_rew05_name;
    } else if (eq_ci(action, "rewind_1")) {
        slot = &cfg->key_rew10;
        name = cfg->key_rew10_name;
        name_n = sizeof cfg->key_rew10_name;
    } else if (eq_ci(action, "rewind_1_5")) {
        slot = &cfg->key_rew15;
        name = cfg->key_rew15_name;
        name_n = sizeof cfg->key_rew15_name;
    } else if (eq_ci(action, "reverb")) {
        slot = &cfg->key_reverb;
        name = cfg->key_reverb_name;
        name_n = sizeof cfg->key_reverb_name;
    } else if (eq_ci(action, "chipmunk") || eq_ci(action, "pitch_shift")) {
        /* pitch_shift is the old name of this button. */
        slot = &cfg->key_pitch;
        name = cfg->key_pitch_name;
        name_n = sizeof cfg->key_pitch_name;
    } else if (eq_ci(action, "autopan")) {
        slot = &cfg->key_autopan;
        name = cfg->key_autopan_name;
        name_n = sizeof cfg->key_autopan_name;
    } else if (eq_ci(action, "phaser")) {
        slot = &cfg->key_phaser;
        name = cfg->key_phaser_name;
        name_n = sizeof cfg->key_phaser_name;
    } else if (eq_ci(action, "chorus")) {
        slot = &cfg->key_chorus;
        name = cfg->key_chorus_name;
        name_n = sizeof cfg->key_chorus_name;
    } else if (eq_ci(action, "flanger")) {
        slot = &cfg->key_flanger;
        name = cfg->key_flanger_name;
        name_n = sizeof cfg->key_flanger_name;
    } else if (eq_ci(action, "effects_off")) {
        slot = &cfg->key_effects_off;
        name = cfg->key_effects_off_name;
        name_n = sizeof cfg->key_effects_off_name;
    } else if (eq_ci(action, "dial_forward") || eq_ci(action, "jog_forward")) {
        slot = &cfg->key_dial_fwd;
        name = cfg->key_dial_fwd_name;
        name_n = sizeof cfg->key_dial_fwd_name;
    } else if (eq_ci(action, "dial_back") || eq_ci(action, "jog_back")) {
        slot = &cfg->key_dial_back;
        name = cfg->key_dial_back_name;
        name_n = sizeof cfg->key_dial_back_name;
    } else if (eq_ci(action, "dial_mode")) {
        slot = &cfg->key_dial_mode;
        name = cfg->key_dial_mode_name;
        name_n = sizeof cfg->key_dial_mode_name;
    } else if (eq_ci(action, "volume_up")) {
        slot = &cfg->key_vol_up;
        name = cfg->key_vol_up_name;
        name_n = sizeof cfg->key_vol_up_name;
    } else if (eq_ci(action, "volume_down")) {
        slot = &cfg->key_vol_down;
        name = cfg->key_vol_down_name;
        name_n = sizeof cfg->key_vol_down_name;
    } else if (eq_ci(action, "jog_up")) {
        slot = &cfg->key_jog_up;
        name = cfg->key_jog_up_name;
        name_n = sizeof cfg->key_jog_up_name;
    } else if (eq_ci(action, "jog_down")) {
        slot = &cfg->key_jog_down;
        name = cfg->key_jog_down_name;
        name_n = sizeof cfg->key_jog_down_name;
    } else {
        log_warn("autodj", "unknown key action %s", action);
        return;
    }
    if (!keyname[0]) {
        *slot = KEY_EXPLICIT_UNBOUND;
        name[0] = '\0';
        return;
    }
    if (key_from_name(keyname, &code) != 0) {
        log_warn("autodj", "unknown key name %s for %s", keyname, action);
        return;
    }
    *slot = code;
    snprintf(name, name_n, "%s", keyname);
}

/* A keyboard action the file does not mention takes the built-in key.
 * A blank or unknown line stays unbound. */
static void bind_kbd_defaults(Config *cfg)
{
    static const char *names[KEYID_COUNT] = {
        "pause_resume_kbd", "skip_kbd", "previous_kbd", "rewind_0_5_kbd",
        "rewind_1_kbd", "rewind_1_5_kbd", "dial_forward_kbd", "dial_back_kbd",
        "dial_mode_kbd", "reverb_kbd", "chipmunk_kbd", "autopan_kbd",
        "phaser_kbd", "chorus_kbd", "flanger_kbd", "effects_off_kbd",
        "volume_up_kbd", "volume_down_kbd", "jog_up_kbd", "jog_down_kbd"
    };
    int id;

    for (id = 0; id < KEYID_COUNT; id++) {
        char *slot = kbd_cfg(cfg, id);

        if (!slot || slot[0] || (cfg->kbd_line & (1u << id))) {
            continue;
        }
        set_key(cfg, names[id], kbd_default(id));
    }
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
    if (eq_ci(section, "Chorus")) {
        return 4;
    }
    if (eq_ci(section, "Flanger")) {
        return 5;
    }
    return -1;
}

static const char *fx_what(int section)
{
    static const char *names[] = {
        "reverb", "chipmunk", "autopan", "phaser", "chorus", "flanger"
    };

    if (section < 0 || section > 5) {
        return "effect";
    }
    return names[section];
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
    const char *what = fx_what(section);

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
    } else if (section == 4 && eq_ci(key, "period")) {
        if (parse_real(val, 0.25, 16, &v) != 0) {
            log_warn("autodj", "chorus period must be from 0.25 to 16, got %s", val);
        } else {
            cfg->fx.ch_period = (float)v;
        }
    } else if (section == 4 && eq_ci(key, "delay")) {
        if (parse_real(val, 5, 40, &v) != 0) {
            log_warn("autodj", "chorus delay must be from 5 to 40 ms, got %s", val);
        } else {
            cfg->fx.ch_delay = (float)v;
        }
    } else if (section == 4 && eq_ci(key, "depth")) {
        if (parse_real(val, 0, 10, &v) != 0) {
            log_warn("autodj", "chorus depth must be from 0 to 10 ms, got %s", val);
        } else {
            cfg->fx.ch_depth = (float)v;
        }
    } else if (section == 4 && eq_ci(key, "voices")) {
        if (parse_real(val, 1, 4, &v) != 0 || v != (int)v) {
            log_warn("autodj", "chorus voices must be a whole number from 1 to 4, got %s", val);
        } else {
            cfg->fx.ch_voices = (int)v;
        }
    } else if (section == 4 && eq_ci(key, "mix")) {
        if (parse_real(val, 0, 100, &v) != 0) {
            log_warn("autodj", "chorus mix must be from 0 to 100, got %s", val);
        } else {
            cfg->fx.ch_mix = (float)v;
        }
    } else if (section == 5 && eq_ci(key, "period")) {
        if (parse_real(val, 0.25, 16, &v) != 0) {
            log_warn("autodj", "flanger period must be from 0.25 to 16, got %s", val);
        } else {
            cfg->fx.fl_period = (float)v;
        }
    } else if (section == 5 && eq_ci(key, "delay")) {
        if (parse_real(val, 0.2, 5, &v) != 0) {
            log_warn("autodj", "flanger delay must be from 0.2 to 5 ms, got %s", val);
        } else {
            cfg->fx.fl_delay = (float)v;
        }
    } else if (section == 5 && eq_ci(key, "depth")) {
        if (parse_real(val, 0, 4, &v) != 0) {
            log_warn("autodj", "flanger depth must be from 0 to 4 ms, got %s", val);
        } else {
            cfg->fx.fl_depth = (float)v;
        }
    } else if (section == 5 && eq_ci(key, "feedback")) {
        if (parse_real(val, 0, 0.9, &v) != 0) {
            log_warn("autodj", "flanger feedback must be from 0 to 0.9, got %s", val);
        } else {
            cfg->fx.fl_feedback = (float)v;
        }
    } else if (section == 5 && eq_ci(key, "mix")) {
        if (parse_real(val, 0, 100, &v) != 0) {
            log_warn("autodj", "flanger mix must be from 0 to 100, got %s", val);
        } else {
            cfg->fx.fl_mix = (float)v;
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
            int volume = 1;

            if (config_parse_dial(val, &volume) != 0) {
                log_warn("autodj", "dial_default must be volume or jog, got %s", val);
            } else {
                cfg->dial_default = volume ? DIAL_VOLUME : DIAL_JOG;
            }
        } else if (eq_ci(key, "dial_mode_1") || eq_ci(key, "dial_mode_2") ||
                   eq_ci(key, "dial_mode_3")) {
            int slot = key[10] - '1';
            int mode = DIAL_NONE;

            if (config_parse_dial_mode(val, &mode) != 0) {
                log_warn("autodj", "%s must be none, volume, or jog, got %s", key, val);
            } else {
                cfg->dial_modes[slot] = mode;
                cfg->dial_mode_line |= 1u << slot;
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
    cfg->dial_modes[0] = DIAL_VOLUME;
    cfg->dial_modes[1] = DIAL_JOG;
    cfg->dial_modes[2] = DIAL_NONE;
    cfg->dial_default = -1;
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
    cfg->key_chorus = -1;
    cfg->key_flanger = -1;
    cfg->key_effects_off = -1;
    cfg->key_vol_up = -1;
    cfg->key_vol_down = -1;
    cfg->key_jog_up = -1;
    cfg->key_jog_down = -1;
    {
        int id;

        for (id = 0; id < KEYID_COUNT; id++) {
            cfg->midi_code[id] = -1;
        }
    }
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
    bind_kbd_defaults(cfg);
    /* An old file sets only where the dial starts. The other mode follows. */
    if (!cfg->dial_mode_line && cfg->dial_default == DIAL_JOG) {
        cfg->dial_modes[0] = DIAL_JOG;
        cfg->dial_modes[1] = DIAL_VOLUME;
    }
    /* Editors and the player treat "no key" as -1. -2 only means the file
     * asked for that, so the defaults above leave it alone. */
    {
        int *slots[] = {
            &cfg->key_pause, &cfg->key_skip, &cfg->key_prev, &cfg->key_rew05,
            &cfg->key_rew10, &cfg->key_rew15, &cfg->key_dial_fwd,
            &cfg->key_dial_back, &cfg->key_dial_mode, &cfg->key_reverb,
            &cfg->key_pitch, &cfg->key_autopan, &cfg->key_phaser,
            &cfg->key_chorus, &cfg->key_flanger, &cfg->key_effects_off,
            &cfg->key_vol_up, &cfg->key_vol_down, &cfg->key_jog_up, &cfg->key_jog_down
        };
        int i;

        for (i = 0; i < (int)(sizeof slots / sizeof slots[0]); i++) {
            if (*slots[i] < -1) {
                *slots[i] = -1;
            }
        }
    }
    cfg->fx.reverb_on = 0;
    cfg->fx.pitch_on = 0;
    cfg->fx.autopan_on = 0;
    cfg->fx.phaser_on = 0;
    cfg->fx.chorus_on = 0;
    cfg->fx.flanger_on = 0;
    if (cfg->fx.ph_low >= cfg->fx.ph_high) {
        FxParams def;

        fx_defaults(&def);
        log_warn("autodj", "phaser low (%g) must be below high (%g), using %g and %g",
                 (double)cfg->fx.ph_low, (double)cfg->fx.ph_high,
                 (double)def.ph_low, (double)def.ph_high);
        cfg->fx.ph_low = def.ph_low;
        cfg->fx.ph_high = def.ph_high;
    }
    if (cfg->fx.ch_depth >= cfg->fx.ch_delay) {
        FxParams def;

        fx_defaults(&def);
        log_warn("autodj", "chorus depth (%g) must be below delay (%g), using %g and %g",
                 (double)cfg->fx.ch_depth, (double)cfg->fx.ch_delay,
                 (double)def.ch_delay, (double)def.ch_depth);
        cfg->fx.ch_delay = def.ch_delay;
        cfg->fx.ch_depth = def.ch_depth;
    }
    if (cfg->fx.fl_depth >= cfg->fx.fl_delay) {
        FxParams def;

        fx_defaults(&def);
        log_warn("autodj", "flanger depth (%g) must be below delay (%g), using %g and %g",
                 (double)cfg->fx.fl_depth, (double)cfg->fx.fl_delay,
                 (double)def.fl_delay, (double)def.fl_depth);
        cfg->fx.fl_delay = def.fl_delay;
        cfg->fx.fl_depth = def.fl_depth;
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
    snprintf(edit->dial_mode[0], sizeof edit->dial_mode[0], "volume");
    snprintf(edit->dial_mode[1], sizeof edit->dial_mode[1], "jog");
    snprintf(edit->dial_mode[2], sizeof edit->dial_mode[2], "none");
    {
        int id;

        for (id = 0; id < KEYID_COUNT; id++) {
            char *slot = kbd_edit(edit, id);

            if (slot) {
                snprintf(slot, AUTODJ_KBD_LEN, "%s", kbd_default(id));
            }
        }
    }
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
    snprintf(edit->ch_period, sizeof edit->ch_period, "1");
    snprintf(edit->ch_delay, sizeof edit->ch_delay, "20");
    snprintf(edit->ch_depth, sizeof edit->ch_depth, "5");
    snprintf(edit->ch_voices, sizeof edit->ch_voices, "2");
    snprintf(edit->ch_mix, sizeof edit->ch_mix, "50");
    snprintf(edit->fl_period, sizeof edit->fl_period, "2");
    snprintf(edit->fl_delay, sizeof edit->fl_delay, "1");
    snprintf(edit->fl_depth, sizeof edit->fl_depth, "0.5");
    snprintf(edit->fl_feedback, sizeof edit->fl_feedback, "0.5");
    snprintf(edit->fl_mix, sizeof edit->fl_mix, "50");
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
        } else if (eq_ci(key, "dial_mode_1") || eq_ci(key, "dial_mode_2") ||
                   eq_ci(key, "dial_mode_3")) {
            int slot = key[10] - '1';
            int mode = DIAL_NONE;

            if (config_parse_dial_mode(val, &mode) == 0) {
                copy_raw(edit->dial_mode[slot], sizeof edit->dial_mode[slot],
                         config_dial_mode_name(mode));
            } else {
                copy_raw(edit->dial_mode[slot], sizeof edit->dial_mode[slot], val);
            }
            edit->dial_mode_line |= 1u << slot;
        }
        return;
    }
    if (eq_ci(section, "Keys")) {
        char base[64];
        int id;

        if (strip_midi(key, base, sizeof base) && (id = keys_id(base)) >= 0) {
            if (midi_canon(val, edit->midi[id], sizeof edit->midi[id]) != 0) {
                copy_raw(edit->midi[id], sizeof edit->midi[id], val);
            }
            return;
        }
        if (strip_kbd(key, base, sizeof base) && (id = keys_id(base)) >= 0) {
            char *slot = kbd_edit(edit, id);

            if (slot) {
                copy_kbd(slot, AUTODJ_KBD_LEN, val);
            }
            return;
        }
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
        } else if (eq_ci(key, "chorus")) {
            copy_raw(edit->key_chorus, sizeof edit->key_chorus, val);
        } else if (eq_ci(key, "flanger")) {
            copy_raw(edit->key_flanger, sizeof edit->key_flanger, val);
        } else if (eq_ci(key, "effects_off")) {
            copy_raw(edit->key_effects_off, sizeof edit->key_effects_off, val);
        } else if (eq_ci(key, "volume_up")) {
            copy_raw(edit->key_vol_up, sizeof edit->key_vol_up, val);
        } else if (eq_ci(key, "volume_down")) {
            copy_raw(edit->key_vol_down, sizeof edit->key_vol_down, val);
        } else if (eq_ci(key, "jog_up")) {
            copy_raw(edit->key_jog_up, sizeof edit->key_jog_up, val);
        } else if (eq_ci(key, "jog_down")) {
            copy_raw(edit->key_jog_down, sizeof edit->key_jog_down, val);
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
        } else if (fx == 4) {
            if (eq_ci(key, "period")) {
                copy_raw(edit->ch_period, sizeof edit->ch_period, val);
            } else if (eq_ci(key, "delay")) {
                copy_raw(edit->ch_delay, sizeof edit->ch_delay, val);
            } else if (eq_ci(key, "depth")) {
                copy_raw(edit->ch_depth, sizeof edit->ch_depth, val);
            } else if (eq_ci(key, "voices")) {
                copy_raw(edit->ch_voices, sizeof edit->ch_voices, val);
            } else if (eq_ci(key, "mix")) {
                copy_raw(edit->ch_mix, sizeof edit->ch_mix, val);
            }
        } else if (fx == 5) {
            if (eq_ci(key, "period")) {
                copy_raw(edit->fl_period, sizeof edit->fl_period, val);
            } else if (eq_ci(key, "delay")) {
                copy_raw(edit->fl_delay, sizeof edit->fl_delay, val);
            } else if (eq_ci(key, "depth")) {
                copy_raw(edit->fl_depth, sizeof edit->fl_depth, val);
            } else if (eq_ci(key, "feedback")) {
                copy_raw(edit->fl_feedback, sizeof edit->fl_feedback, val);
            } else if (eq_ci(key, "mix")) {
                copy_raw(edit->fl_mix, sizeof edit->fl_mix, val);
            }
        }
    }
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
    {
        int volume = 1;

        /* An old file sets only where the dial starts. The other mode follows. */
        if (!edit->dial_mode_line && config_parse_dial(edit->dial_default, &volume) == 0 &&
            !volume) {
            snprintf(edit->dial_mode[0], sizeof edit->dial_mode[0], "jog");
            snprintf(edit->dial_mode[1], sizeof edit->dial_mode[1], "volume");
        }
    }
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

int config_parse_dial_mode(const char *s, int *mode)
{
    if (!s || !mode) {
        return -1;
    }
    if (eq_ci(s, "none")) {
        *mode = DIAL_NONE;
        return 0;
    }
    if (eq_ci(s, "volume")) {
        *mode = DIAL_VOLUME;
        return 0;
    }
    if (eq_ci(s, "jog")) {
        *mode = DIAL_JOG;
        return 0;
    }
    return -1;
}

const char *config_dial_mode_name(int mode)
{
    if (mode == DIAL_VOLUME) {
        return "volume";
    }
    if (mode == DIAL_JOG) {
        return "jog";
    }
    return "none";
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

static int take_kbd(char *err, size_t err_n, const char *text, const char *what,
                    char *dest, size_t dest_n)
{
    char canon[AUTODJ_KBD_LEN];
    char msg[192];

    if (!text[0]) {
        dest[0] = '\0';
        return 0;
    }
    if (keys_kbd_canon(text, canon, sizeof canon) != 0) {
        snprintf(msg, sizeof msg,
                 "%s keyboard key \"%s\" is not a character or a key name.",
                 what, text);
        add_err(err, err_n, msg);
        return -1;
    }
    snprintf(dest, dest_n, "%s", canon);
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

static int take_midi(char *err, size_t err_n, const char *text, const char *what,
                     char *dest, size_t dest_n, int *code)
{
    char canon[AUTODJ_MIDI_LEN];

    *code = -1;
    if (!text[0]) {
        dest[0] = '\0';
        return 0;
    }
    if (midi_canon(text, canon, sizeof canon) != 0 || midi_from_name(canon, code) != 0) {
        char msg[192];

        snprintf(msg, sizeof msg, "%s MIDI control \"%s\" is not NOTE_<n> or CC_<n>_UP/DOWN.",
                 what, text);
        add_err(err, err_n, msg);
        *code = -1;
        return -1;
    }
    snprintf(dest, dest_n, "%s", canon);
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
    int kbd_ok[KEYID_COUNT];
    int midi_ok[KEYID_COUNT];
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
    trim(in.key_chorus);
    trim(in.key_flanger);
    trim(in.key_effects_off);
    trim(in.key_vol_up);
    trim(in.key_vol_down);
    trim(in.key_jog_up);
    trim(in.key_jog_down);
    for (i = 0; i < DIAL_SLOTS; i++) {
        trim(in.dial_mode[i]);
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        char *kbd = kbd_edit(&in, i);

        if (kbd) {
            trim(kbd);
        }
        trim(in.midi[i]);
    }
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
    trim(in.ch_period);
    trim(in.ch_delay);
    trim(in.ch_depth);
    trim(in.ch_voices);
    trim(in.ch_mix);
    trim(in.fl_period);
    trim(in.fl_delay);
    trim(in.fl_depth);
    trim(in.fl_feedback);
    trim(in.fl_mix);

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
    out.key_chorus = -1;
    out.key_flanger = -1;
    out.key_effects_off = -1;
    out.key_vol_up = -1;
    out.key_vol_down = -1;
    out.key_jog_up = -1;
    out.key_jog_down = -1;
    out.dial_default = -1;
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
    out.dial_modes[0] = DIAL_VOLUME;
    out.dial_modes[1] = DIAL_JOG;
    out.dial_modes[2] = DIAL_NONE;
    out.dial_mode_line = (1u << DIAL_SLOTS) - 1;
    for (i = 0; i < DIAL_SLOTS; i++) {
        if (in.dial_mode[i][0] && config_parse_dial_mode(in.dial_mode[i], &out.dial_modes[i]) != 0) {
            char msg[96];

            snprintf(msg, sizeof msg, "Dial mode %d must be none, volume, or jog.", i + 1);
            add_err(err, err_n, msg);
            failed = 1;
        }
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
    key_ok[KEYID_CHORUS] = take_key(err, err_n, in.key_chorus, "Chorus",
                                    &out.key_chorus, out.key_chorus_name,
                                    sizeof out.key_chorus_name) == 0;
    key_ok[KEYID_FLANGER] = take_key(err, err_n, in.key_flanger, "Flanger",
                                     &out.key_flanger, out.key_flanger_name,
                                     sizeof out.key_flanger_name) == 0;
    key_ok[KEYID_EFFECTS_OFF] = take_key(err, err_n, in.key_effects_off, "All effects off",
                                         &out.key_effects_off, out.key_effects_off_name,
                                         sizeof out.key_effects_off_name) == 0;
    key_ok[KEYID_VOL_UP] = take_key(err, err_n, in.key_vol_up, "Volume up",
                                    &out.key_vol_up, out.key_vol_up_name,
                                    sizeof out.key_vol_up_name) == 0;
    key_ok[KEYID_VOL_DOWN] = take_key(err, err_n, in.key_vol_down, "Volume down",
                                      &out.key_vol_down, out.key_vol_down_name,
                                      sizeof out.key_vol_down_name) == 0;
    key_ok[KEYID_JOG_UP] = take_key(err, err_n, in.key_jog_up, "Jog up",
                                    &out.key_jog_up, out.key_jog_up_name,
                                    sizeof out.key_jog_up_name) == 0;
    key_ok[KEYID_JOG_DOWN] = take_key(err, err_n, in.key_jog_down, "Jog down",
                                      &out.key_jog_down, out.key_jog_down_name,
                                      sizeof out.key_jog_down_name) == 0;
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
    keys[KEYID_CHORUS] = in.key_chorus;
    keys[KEYID_FLANGER] = in.key_flanger;
    keys[KEYID_EFFECTS_OFF] = in.key_effects_off;
    keys[KEYID_VOL_UP] = in.key_vol_up;
    keys[KEYID_VOL_DOWN] = in.key_vol_down;
    keys[KEYID_JOG_UP] = in.key_jog_up;
    keys[KEYID_JOG_DOWN] = in.key_jog_down;
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
    labels[KEYID_CHORUS] = "chorus";
    labels[KEYID_FLANGER] = "flanger";
    labels[KEYID_EFFECTS_OFF] = "all effects off";
    labels[KEYID_VOL_UP] = "volume up";
    labels[KEYID_VOL_DOWN] = "volume down";
    labels[KEYID_JOG_UP] = "jog up";
    labels[KEYID_JOG_DOWN] = "jog down";
    kbd_ok[KEYID_PAUSE] = take_kbd(err, err_n, in.kbd_pause, "Pause/resume",
                                   out.kbd_pause, sizeof out.kbd_pause) == 0;
    kbd_ok[KEYID_SKIP] = take_kbd(err, err_n, in.kbd_skip, "Skip",
                                  out.kbd_skip, sizeof out.kbd_skip) == 0;
    kbd_ok[KEYID_PREV] = take_kbd(err, err_n, in.kbd_prev, "Previous",
                                  out.kbd_prev, sizeof out.kbd_prev) == 0;
    kbd_ok[KEYID_REW05] = take_kbd(err, err_n, in.kbd_rew05, "Rewind 0.5s",
                                   out.kbd_rew05, sizeof out.kbd_rew05) == 0;
    kbd_ok[KEYID_REW10] = take_kbd(err, err_n, in.kbd_rew10, "Rewind 1s",
                                   out.kbd_rew10, sizeof out.kbd_rew10) == 0;
    kbd_ok[KEYID_REW15] = take_kbd(err, err_n, in.kbd_rew15, "Rewind 1.5s",
                                   out.kbd_rew15, sizeof out.kbd_rew15) == 0;
    kbd_ok[KEYID_DIAL_FWD] = take_kbd(err, err_n, in.kbd_dial_fwd, "Dial forward",
                                      out.kbd_dial_fwd, sizeof out.kbd_dial_fwd) == 0;
    kbd_ok[KEYID_DIAL_BACK] = take_kbd(err, err_n, in.kbd_dial_back, "Dial back",
                                       out.kbd_dial_back, sizeof out.kbd_dial_back) == 0;
    kbd_ok[KEYID_DIAL_MODE] = take_kbd(err, err_n, in.kbd_dial_mode, "Dial mode",
                                       out.kbd_dial_mode, sizeof out.kbd_dial_mode) == 0;
    kbd_ok[KEYID_REVERB] = take_kbd(err, err_n, in.kbd_reverb, "Reverb",
                                    out.kbd_reverb, sizeof out.kbd_reverb) == 0;
    kbd_ok[KEYID_PITCH] = take_kbd(err, err_n, in.kbd_pitch, "Chipmunk",
                                   out.kbd_pitch, sizeof out.kbd_pitch) == 0;
    kbd_ok[KEYID_AUTOPAN] = take_kbd(err, err_n, in.kbd_autopan, "Autopan",
                                     out.kbd_autopan, sizeof out.kbd_autopan) == 0;
    kbd_ok[KEYID_PHASER] = take_kbd(err, err_n, in.kbd_phaser, "Phaser",
                                    out.kbd_phaser, sizeof out.kbd_phaser) == 0;
    kbd_ok[KEYID_CHORUS] = take_kbd(err, err_n, in.kbd_chorus, "Chorus",
                                    out.kbd_chorus, sizeof out.kbd_chorus) == 0;
    kbd_ok[KEYID_FLANGER] = take_kbd(err, err_n, in.kbd_flanger, "Flanger",
                                     out.kbd_flanger, sizeof out.kbd_flanger) == 0;
    kbd_ok[KEYID_EFFECTS_OFF] = take_kbd(err, err_n, in.kbd_effects_off, "All effects off",
                                         out.kbd_effects_off, sizeof out.kbd_effects_off) == 0;
    kbd_ok[KEYID_VOL_UP] = take_kbd(err, err_n, in.kbd_vol_up, "Volume up",
                                    out.kbd_vol_up, sizeof out.kbd_vol_up) == 0;
    kbd_ok[KEYID_VOL_DOWN] = take_kbd(err, err_n, in.kbd_vol_down, "Volume down",
                                      out.kbd_vol_down, sizeof out.kbd_vol_down) == 0;
    kbd_ok[KEYID_JOG_UP] = take_kbd(err, err_n, in.kbd_jog_up, "Jog up",
                                    out.kbd_jog_up, sizeof out.kbd_jog_up) == 0;
    kbd_ok[KEYID_JOG_DOWN] = take_kbd(err, err_n, in.kbd_jog_down, "Jog down",
                                      out.kbd_jog_down, sizeof out.kbd_jog_down) == 0;
    for (i = 0; i < KEYID_COUNT; i++) {
        char what[32];

        snprintf(what, sizeof what, "%c%s", toupper((unsigned char)labels[i][0]), labels[i] + 1);
        midi_ok[i] = take_midi(err, err_n, in.midi[i], what, out.midi[i], sizeof out.midi[i],
                               &out.midi_code[i]) == 0;
    }
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
    {
        int delay_ok = !in.ch_delay[0] ||
                       take_real(err, err_n, in.ch_delay, 5, 40, "Chorus delay",
                                 &out.fx.ch_delay) == 0;
        int depth_ok = !in.ch_depth[0] ||
                       take_real(err, err_n, in.ch_depth, 0, 10, "Chorus depth",
                                 &out.fx.ch_depth) == 0;

        if (in.ch_period[0] &&
            take_real(err, err_n, in.ch_period, 0.25, 16, "Chorus period",
                      &out.fx.ch_period) != 0) {
            failed = 1;
        }
        if (!delay_ok || !depth_ok) {
            failed = 1;
        } else if (out.fx.ch_depth >= out.fx.ch_delay) {
            add_err(err, err_n, "Chorus depth must be less than delay.");
            failed = 1;
        }
        if (in.ch_voices[0] &&
            take_int(err, err_n, in.ch_voices, 1, 4, "Voices", &out.fx.ch_voices) != 0) {
            failed = 1;
        }
        if (in.ch_mix[0] &&
            take_real(err, err_n, in.ch_mix, 0, 100, "Chorus mix", &out.fx.ch_mix) != 0) {
            failed = 1;
        }
    }
    {
        int delay_ok = !in.fl_delay[0] ||
                       take_real(err, err_n, in.fl_delay, 0.2, 5, "Flanger delay",
                                 &out.fx.fl_delay) == 0;
        int depth_ok = !in.fl_depth[0] ||
                       take_real(err, err_n, in.fl_depth, 0, 4, "Flanger depth",
                                 &out.fx.fl_depth) == 0;

        if (in.fl_period[0] &&
            take_real(err, err_n, in.fl_period, 0.25, 16, "Flanger period",
                      &out.fx.fl_period) != 0) {
            failed = 1;
        }
        if (!delay_ok || !depth_ok) {
            failed = 1;
        } else if (out.fx.fl_depth >= out.fx.fl_delay) {
            add_err(err, err_n, "Flanger depth must be less than delay.");
            failed = 1;
        }
        if (in.fl_feedback[0] &&
            take_real(err, err_n, in.fl_feedback, 0, 0.9, "Flanger feedback",
                      &out.fx.fl_feedback) != 0) {
            failed = 1;
        }
        if (in.fl_mix[0] &&
            take_real(err, err_n, in.fl_mix, 0, 100, "Flanger mix", &out.fx.fl_mix) != 0) {
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
    for (i = 0; i < KEYID_COUNT; i++) {
        if (!kbd_ok[i]) {
            failed = 1;
        }
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        for (j = i + 1; j < KEYID_COUNT; j++) {
            char msg[192];
            const char *a = kbd_cfg(&out, i);
            const char *b = kbd_cfg(&out, j);

            if (!kbd_ok[i] || !kbd_ok[j] || !a || !b || !a[0] || strcmp(a, b) != 0) {
                continue;
            }
            snprintf(msg, sizeof msg, "%s is used for %s keyboard and %s keyboard.",
                     a, labels[i], labels[j]);
            add_err(err, err_n, msg);
            failed = 1;
        }
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        if (!midi_ok[i]) {
            failed = 1;
        }
    }
    for (i = 0; i < KEYID_COUNT; i++) {
        for (j = i + 1; j < KEYID_COUNT; j++) {
            char msg[192];

            if (!midi_ok[i] || !midi_ok[j] || !out.midi[i][0] ||
                strcmp(out.midi[i], out.midi[j]) != 0) {
                continue;
            }
            snprintf(msg, sizeof msg, "%s is used for %s MIDI and %s MIDI.",
                     out.midi[i], labels[i], labels[j]);
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
    if (eq_ci(key, "dial_mode_1")) {
        return 9;
    }
    if (eq_ci(key, "dial_mode_2")) {
        return 10;
    }
    if (eq_ci(key, "dial_mode_3")) {
        return 11;
    }
    return -1;
}

enum { MAIN_COUNT = 12 };

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
    if (eq_ci(key, "chorus")) {
        return KEYID_CHORUS;
    }
    if (eq_ci(key, "flanger")) {
        return KEYID_FLANGER;
    }
    if (eq_ci(key, "effects_off")) {
        return KEYID_EFFECTS_OFF;
    }
    if (eq_ci(key, "volume_up")) {
        return KEYID_VOL_UP;
    }
    if (eq_ci(key, "volume_down")) {
        return KEYID_VOL_DOWN;
    }
    if (eq_ci(key, "jog_up")) {
        return KEYID_JOG_UP;
    }
    if (eq_ci(key, "jog_down")) {
        return KEYID_JOG_DOWN;
    }
    return -1;
}

static int kbd_id(const char *key)
{
    char base[64];

    if (!strip_kbd(key, base, sizeof base)) {
        return -1;
    }
    return keys_id(base);
}

static int midi_id(const char *key)
{
    char base[64];

    if (!strip_midi(key, base, sizeof base)) {
        return -1;
    }
    return keys_id(base);
}

static const char *g_jog_names[] = {
    "seconds_per_turn", "clicks_per_turn", "max_speed", "spinup_time", "grab_time", "smoothing"
};

enum { JOG_COUNT = 6 };

static void fmt_float(char *dest, size_t n, float v)
{
    snprintf(dest, n, "%g", (double)v);
}

static const char *const g_fx_keys[6][7] = {
    {"decay", "bw", "damping", "send"},
    {"pitch", "range", "semitones", "formant"},
    {"period", "smoothing", "distance"},
    {"period", "low", "high", "stages", "feedback", "mix", "boost"},
    {"period", "delay", "depth", "voices", "mix"},
    {"period", "delay", "depth", "feedback", "mix"}
};

static const int g_fx_key_count[6] = {4, 4, 3, 7, 5, 5};

static int fx_key_id(int section, const char *key)
{
    int i;

    if (section < 0 || section > 5) {
        return -1;
    }
    for (i = 0; i < g_fx_key_count[section]; i++) {
        if (eq_ci(key, g_fx_keys[section][i]) ||
            (section == 0 && i == 1 && eq_ci(key, "bandwidth"))) {
            return i;
        }
    }
    return -1;
}

static const char *fx_key_name(int section, int id)
{
    if (section < 0 || section > 5 || id < 0 || id >= g_fx_key_count[section]) {
        return "";
    }
    return g_fx_keys[section][id];
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
    if (section == 4) {
        switch (id) {
        case 0:
            fmt_float(dest, n, fx->ch_period);
            return;
        case 1:
            fmt_float(dest, n, fx->ch_delay);
            return;
        case 2:
            fmt_float(dest, n, fx->ch_depth);
            return;
        case 3:
            snprintf(dest, n, "%d", fx->ch_voices);
            return;
        default:
            fmt_float(dest, n, fx->ch_mix);
            return;
        }
    }
    if (section == 5) {
        switch (id) {
        case 0:
            fmt_float(dest, n, fx->fl_period);
            return;
        case 1:
            fmt_float(dest, n, fx->fl_delay);
            return;
        case 2:
            fmt_float(dest, n, fx->fl_depth);
            return;
        case 3:
            fmt_float(dest, n, fx->fl_feedback);
            return;
        default:
            fmt_float(dest, n, fx->fl_mix);
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
        snprintf(dest, n, "%s", config_dial_mode_name(cfg->dial_modes[(id - 9) % DIAL_SLOTS]));
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
    case KEYID_CHORUS:
        s = cfg->key_chorus_name;
        break;
    case KEYID_FLANGER:
        s = cfg->key_flanger_name;
        break;
    case KEYID_EFFECTS_OFF:
        s = cfg->key_effects_off_name;
        break;
    case KEYID_VOL_UP:
        s = cfg->key_vol_up_name;
        break;
    case KEYID_VOL_DOWN:
        s = cfg->key_vol_down_name;
        break;
    case KEYID_JOG_UP:
        s = cfg->key_jog_up_name;
        break;
    case KEYID_JOG_DOWN:
        s = cfg->key_jog_down_name;
        break;
    default:
        break;
    }
    snprintf(dest, n, "%s", s ? s : "");
}

static void midi_value(const Config *cfg, int id, char *dest, size_t n)
{
    snprintf(dest, n, "%s", id >= 0 && id < KEYID_COUNT ? cfg->midi[id] : "");
}

static void kbd_value(const Config *cfg, int id, char *dest, size_t n)
{
    const char *s = kbd_cfg((Config *)cfg, id);

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
    static const char *main_names[MAIN_COUNT] = {
        "Folder", "shuffle", "overlap", "trim_silence", "pause_time",
        "resume_time", "volume", "loudness", "loudness_lufs",
        "dial_mode_1", "dial_mode_2", "dial_mode_3"
    };
    static const char *key_names[KEYID_COUNT] = {
        "pause_resume", "skip", "previous", "rewind_0_5", "rewind_1",
        "rewind_1_5", "dial_forward", "dial_back", "dial_mode",
        "reverb", "chipmunk", "autopan", "phaser", "chorus", "flanger",
        "effects_off", "volume_up", "volume_down", "jog_up", "jog_down"
    };
    static const char *kbd_names[KEYID_COUNT] = {
        "pause_resume_kbd", "skip_kbd", "previous_kbd", "rewind_0_5_kbd",
        "rewind_1_kbd", "rewind_1_5_kbd", "dial_forward_kbd", "dial_back_kbd",
        "dial_mode_kbd", "reverb_kbd", "chipmunk_kbd", "autopan_kbd",
        "phaser_kbd", "chorus_kbd", "flanger_kbd", "effects_off_kbd",
        "volume_up_kbd", "volume_down_kbd", "jog_up_kbd", "jog_down_kbd"
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
    int main_seen[MAIN_COUNT] = {0};
    int keys_seen[KEYID_COUNT] = {0};
    int kbd_seen[KEYID_COUNT] = {0};
    int midi_seen[KEYID_COUNT] = {0};
    int fx_seen[6][7] = {{0}};
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
            (eq_ci(key, "volume_dn") || eq_ci(key, "brake_delayvolume_up"))) {
            unlink_line(&head, prev, line);
            line = next;
            continue;
        }
        /* dial_mode_1 to dial_mode_3 replace where the dial starts. */
        if (!jog && in_main && line_setting_key(line->text, key, sizeof key) &&
            eq_ci(key, "dial_default")) {
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
            int bid = -1;
            int mid = -1;
            int is_main = in_main && (id = main_id(key)) >= 0;
            int is_key = in_keys && (id = keys_id(key)) >= 0;
            int is_kbd = in_keys && (bid = kbd_id(key)) >= 0;
            int is_midi = in_keys && (mid = midi_id(key)) >= 0;

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
                /* An empty value stays on a line that is already there. */
                if (rewrite_value(line, value) != 0) {
                    set_err(err, err_n, "out of memory updating %s", path);
                    goto done;
                }
            } else if (is_kbd) {
                kbd_seen[bid] = 1;
                kbd_value(cfg, bid, value, sizeof value);
                if (rewrite_value(line, value) != 0) {
                    set_err(err, err_n, "out of memory updating %s", path);
                    goto done;
                }
            } else if (is_midi) {
                midi_seen[mid] = 1;
                midi_value(cfg, mid, value, sizeof value);
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
    for (id = 0; id < MAIN_COUNT && !jog; id++) {
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

    for (id = 0; id < KEYID_COUNT && !jog; id++) {
        if (!kbd_seen[id]) {
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

            if (kbd_seen[id]) {
                continue;
            }
            kbd_value(cfg, id, value, sizeof value);
            /* A blank line stays in the file so the default does not return. */
            if (snprintf(text, sizeof text, "%s=%s\n", kbd_names[id], value) >= (int)sizeof text) {
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

    /* MIDI lines are written only when bound, like the keypad. */
    for (id = 0; id < KEYID_COUNT && !jog; id++) {
        char text[160];

        if (midi_seen[id] || !cfg->midi[id][0]) {
            continue;
        }
        if (ensure_section(&head, "Keys", &keys_header, &keys_last) != 0) {
            set_err(err, err_n, "out of memory updating %s", path);
            goto done;
        }
        snprintf(text, sizeof text, "%s_midi=%s\n", key_names[id], cfg->midi[id]);
        keys_last = insert_after(&head, keys_last, text);
        if (!keys_last) {
            set_err(err, err_n, "out of memory updating %s", path);
            goto done;
        }
    }

    if (!jog) {
        static const char *fx_names[] = {
            "Reverb", "Chipmunk", "Autopan", "Phaser", "Chorus", "Flanger"
        };
        int fx;

        for (fx = 0; fx < 6; fx++) {
            struct CfgLine *fx_header = NULL;
            struct CfgLine *fx_last = NULL;

            if (ensure_section(&head, fx_names[fx], &fx_header, &fx_last) != 0) {
                set_err(err, err_n, "out of memory updating %s", path);
                goto done;
            }
            for (id = 0; id < g_fx_key_count[fx]; id++) {
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
        "volume_dn=KEY_KPMINUS\n"
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
        "dial_mode_1=volume\n"
        "dial_mode_2=jog\n"
        "dial_mode_3=none\n"
        "\n"
        "[Keys]\n"
        "# key comment\n"
        "skip=KEY_KPDOT\n"
        "custom=KEY_A\n"
        "phaser=KEY_B\n"
        "pause_resume=KEY_KP0\n"
        "chipmunk=KEY_C\n"
        "pause_resume_kbd=\n"
        "skip_kbd=\n"
        "previous_kbd=\n"
        "rewind_0_5_kbd=\n"
        "rewind_1_kbd=\n"
        "rewind_1_5_kbd=\n"
        "dial_forward_kbd=\n"
        "dial_back_kbd=\n"
        "dial_mode_kbd=\n"
        "reverb_kbd=\n"
        "chipmunk_kbd=\n"
        "autopan_kbd=\n"
        "phaser_kbd=\n"
        "chorus_kbd=\n"
        "flanger_kbd=\n"
        "effects_off_kbd=\n"
        "volume_up_kbd=\n"
        "volume_down_kbd=\n"
        "jog_up_kbd=\n"
        "jog_down_kbd=\n"
        "volume_up_midi=CC_3_UP\n"
        "volume_down_midi=CC_3_DOWN\n"
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
        "distance=75\n"
        "[Chorus]\n"
        "period=1\n"
        "delay=20\n"
        "depth=5\n"
        "voices=2\n"
        "mix=50\n"
        "[Flanger]\n"
        "period=2\n"
        "delay=1\n"
        "depth=0.5\n"
        "feedback=0.5\n"
        "mix=50\n";
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
    cfg.dial_modes[0] = DIAL_VOLUME;
    cfg.dial_modes[1] = DIAL_JOG;
    cfg.dial_modes[2] = DIAL_NONE;
    snprintf(cfg.midi[KEYID_VOL_UP], AUTODJ_MIDI_LEN, "CC_3_UP");
    snprintf(cfg.midi[KEYID_VOL_DOWN], AUTODJ_MIDI_LEN, "CC_3_DOWN");
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
    cfg.key_rew15_name[0] = '\0';
    if (rc == 0 && (config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 ||
                    strstr(got, "volume_dn=") != NULL ||
                    strstr(got, "skip=\n") == NULL ||
                    strstr(got, "skip=KEY_KPDOT") != NULL ||
                    strstr(got, "pause_resume=KEY_KP0\n") == NULL ||
                    strstr(got, "phaser=KEY_B\n") == NULL ||
                    strstr(got, "autopan=") != NULL ||
                    strstr(got, "custom=KEY_A\n") == NULL ||
                    config_load(path, &cfg) != 0 || cfg.key_skip != -1 ||
                    cfg.key_rew15 != -1 || cfg.key_pause != 82)) {
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
                    edit.key_prev[0] != '\0' ||
                    edit.key_rew05[0] != '\0' ||
                    edit.key_rew10[0] != '\0' ||
                    edit.key_rew15[0] != '\0' ||
                    strcmp(edit.rv_decay, "0.5") != 0 ||
                    strcmp(edit.ps_semi, "true") != 0 ||
                    strcmp(edit.ap_period, "4") != 0 ||
                    strcmp(edit.ap_smooth, "0.5") != 0 ||
                    strcmp(edit.ch_delay, "20") != 0 ||
                    strcmp(edit.ch_voices, "2") != 0 ||
                    strcmp(edit.fl_delay, "1") != 0 ||
                    strcmp(edit.fl_depth, "0.5") != 0 ||
                    edit.key_reverb[0] != '\0' ||
                    edit.key_chorus[0] != '\0' ||
                    edit.key_flanger[0] != '\0' ||
                    edit.key_effects_off[0] != '\0' ||
                    strcmp(edit.kbd_pause, "Space") != 0 ||
                    strcmp(edit.kbd_skip, "Right") != 0 ||
                    strcmp(edit.kbd_prev, "Left") != 0 ||
                    strcmp(edit.kbd_rew05, "1") != 0 ||
                    strcmp(edit.kbd_rew10, "2") != 0 ||
                    strcmp(edit.kbd_rew15, "3") != 0 ||
                    strcmp(edit.kbd_dial_fwd, "Up") != 0 ||
                    strcmp(edit.kbd_dial_back, "Down") != 0 ||
                    strcmp(edit.kbd_dial_mode, "Enter") != 0 ||
                    strcmp(edit.kbd_reverb, "q") != 0 ||
                    strcmp(edit.kbd_pitch, "w") != 0 ||
                    strcmp(edit.kbd_autopan, "e") != 0 ||
                    strcmp(edit.kbd_phaser, "r") != 0 ||
                    strcmp(edit.kbd_chorus, "t") != 0 ||
                    strcmp(edit.kbd_flanger, "y") != 0 ||
                    strcmp(edit.kbd_effects_off, "Tab") != 0)) {
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
                    cfg.fx.ph_low != 300 || cfg.fx.ph_stages != 6 ||
                    cfg.fx.ch_delay != 20 || cfg.fx.ch_voices != 2 ||
                    cfg.fx.fl_delay != 1 || cfg.fx.fl_depth != 0.5f ||
                    cfg.key_chorus != -1 || cfg.key_flanger != -1 ||
                    cfg.key_effects_off != -1)) {
        log_error("autodj", "config editor test rejected a valid form: %s", err);
        rc = -1;
    }
    snprintf(edit.ph_low, sizeof edit.ph_low, "3000");
    snprintf(edit.ph_high, sizeof edit.ph_high, "3000");
    snprintf(edit.ph_stages, sizeof edit.ph_stages, "2.5");
    snprintf(edit.ch_delay, sizeof edit.ch_delay, "10");
    snprintf(edit.ch_depth, sizeof edit.ch_depth, "10");
    snprintf(edit.ch_voices, sizeof edit.ch_voices, "2.5");
    snprintf(edit.fl_delay, sizeof edit.fl_delay, "0.5");
    snprintf(edit.fl_depth, sizeof edit.fl_depth, "0.5");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
                    strstr(err, "Low Hz must be below High Hz.") == NULL ||
                    strstr(err, "Stages must be a whole number from 1 to 12.") == NULL ||
                    strstr(err, "Chorus depth must be less than delay.") == NULL ||
                    strstr(err, "Voices must be a whole number from 1 to 4.") == NULL ||
                    strstr(err, "Flanger depth must be less than delay.") == NULL)) {
        log_error("autodj", "config editor test missed a bad effect: %s", err);
        rc = -1;
    }
    edit.ph_low[0] = '\0';
    edit.ph_high[0] = '\0';
    edit.ph_stages[0] = '\0';
    edit.ch_delay[0] = '\0';
    edit.ch_depth[0] = '\0';
    edit.ch_voices[0] = '\0';
    edit.fl_delay[0] = '\0';
    edit.fl_depth[0] = '\0';
    snprintf(edit.volume, sizeof edit.volume, "150");
    snprintf(edit.folder, sizeof edit.folder, "/tmp/autodj-missing-folder");
    snprintf(edit.shuffle, sizeof edit.shuffle, "maybe");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KP0");
    snprintf(edit.key_effects_off, sizeof edit.key_effects_off, "KEY_KP0");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
                    strstr(err, "Folder does not exist.") == NULL ||
                    strstr(err, "Shuffle must be true or false.") == NULL ||
                    strstr(err, "Volume must be a whole number from 0 to 100.") == NULL ||
                    strstr(err, "KEY_KP0 is used for pause/resume and skip.") == NULL ||
                    strstr(err, "all effects off") == NULL)) {
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

static int kbd_config_self_test(void)
{
    char path[] = "/tmp/autodj-kbd-XXXXXX";
    char err[512];
    char got[4096];
    Config cfg;
    ConfigEdit edit;
    int fd;
    FILE *fp;

    fd = mkstemp(path);
    if (fd < 0) {
        log_error("autodj", "keyboard config test could not create a temp file");
        return -1;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        log_error("autodj", "keyboard config test could not write a temp file");
        close(fd);
        remove(path);
        return -1;
    }
    fputs("[Main]\n"
          "Folder=/tmp\n"
          "[Keys]\n"
          "pause_resume=KEY_KP0\n"
          "reverb_kbd=R\n"
          "skip_kbd=space\n"
          "pause_resume_kbd=KEY_A\n"
          "effects_off_kbd=\n",
          fp);
    fclose(fp);

    if (config_load(path, &cfg) != 0 || cfg.key_pause != 82 ||
        strcmp(cfg.kbd_reverb, "R") != 0 || strcmp(cfg.kbd_skip, "Space") != 0 ||
        cfg.kbd_pause[0] != '\0' || cfg.kbd_effects_off[0] != '\0' ||
        strcmp(cfg.kbd_prev, "Left") != 0 || strcmp(cfg.kbd_pitch, "w") != 0 ||
        strcmp(cfg.kbd_chorus, "t") != 0) {
        log_error("autodj", "keyboard config test did not load the bindings");
        remove(path);
        return -1;
    }
    if (config_load_edit(path, &edit) != 0 ||
        strcmp(edit.kbd_reverb, "R") != 0 || strcmp(edit.kbd_skip, "Space") != 0 ||
        strcmp(edit.kbd_pause, "KEY_A") != 0 || edit.kbd_effects_off[0] != '\0' ||
        strcmp(edit.kbd_prev, "Left") != 0 || strcmp(edit.key_pause, "KEY_KP0") != 0) {
        log_error("autodj", "keyboard config test did not load the editor text");
        remove(path);
        return -1;
    }
    if (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
        strstr(err, "KEY_A") == NULL) {
        log_error("autodj", "keyboard config test accepted KEY_A: %s", err);
        remove(path);
        return -1;
    }
    snprintf(edit.kbd_pause, sizeof edit.kbd_pause, "p");
    snprintf(edit.kbd_skip, sizeof edit.kbd_skip, "p");
    if (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
        strstr(err, "p is used for pause/resume keyboard and skip keyboard.") == NULL) {
        log_error("autodj", "keyboard config test missed a duplicate: %s", err);
        remove(path);
        return -1;
    }
    snprintf(edit.kbd_skip, sizeof edit.kbd_skip, "P");
    if (config_validate_edit(&edit, &cfg, err, sizeof err) != 0 ||
        strcmp(cfg.kbd_pause, "p") != 0 || strcmp(cfg.kbd_skip, "P") != 0 ||
        strcmp(cfg.kbd_reverb, "R") != 0 || cfg.kbd_effects_off[0] != '\0' ||
        cfg.key_pause != 82) {
        log_error("autodj", "keyboard config test rejected p and P: %s", err);
        remove(path);
        return -1;
    }
    if (config_save(path, &cfg, err, sizeof err) != 0 ||
        read_text(path, got, sizeof got) != 0 ||
        strstr(got, "pause_resume=KEY_KP0\n") == NULL ||
        strstr(got, "reverb_kbd=R\n") == NULL ||
        strstr(got, "skip_kbd=P\n") == NULL ||
        strstr(got, "pause_resume_kbd=p\n") == NULL ||
        strstr(got, "effects_off_kbd=\n") == NULL ||
        strstr(got, "previous_kbd=Left\n") == NULL ||
        strstr(got, "chorus_kbd=t\n") == NULL) {
        log_error("autodj", "keyboard config test saved unexpected lines\n%s", got);
        remove(path);
        return -1;
    }
    cfg.kbd_reverb[0] = '\0';
    if (config_save(path, &cfg, err, sizeof err) != 0 ||
        read_text(path, got, sizeof got) != 0 ||
        strstr(got, "reverb_kbd=\n") == NULL ||
        strstr(got, "reverb_kbd=R") != NULL ||
        config_load(path, &cfg) != 0 || cfg.kbd_reverb[0] != '\0' ||
        strcmp(cfg.kbd_pause, "p") != 0 || strcmp(cfg.kbd_skip, "P") != 0) {
        log_error("autodj", "keyboard config test did not clear a binding\n%s", got);
        remove(path);
        return -1;
    }
    remove(path);
    return 0;
}

static int midi_config_self_test(void)
{
    char path[] = "/tmp/autodj-midi-XXXXXX";
    char err[1024];
    char got[4096];
    Config cfg;
    ConfigEdit edit;
    int fd;
    FILE *fp;
    int rc = -1;

    fd = mkstemp(path);
    if (fd < 0 || !(fp = fdopen(fd, "w"))) {
        log_error("autodj", "MIDI config test could not write a temp file");
        if (fd >= 0) {
            close(fd);
            remove(path);
        }
        return -1;
    }
    fputs("[Main]\n"
          "Folder=/tmp\n"
          "dial_mode_1=jog\n"
          "dial_mode_2=none\n"
          "dial_default=volume\n"
          "[Keys]\n"
          "skip_midi=note_36\n"
          "volume_up_midi=CC_3_UP\n"
          "volume_down_midi=cc_3_down\n"
          "pause_resume_midi=KEY_A\n"
          "jog_up=KEY_KP8\n",
          fp);
    fclose(fp);
    if (config_load(path, &cfg) != 0 || strcmp(cfg.midi[KEYID_SKIP], "NOTE_36") != 0 ||
        cfg.midi_code[KEYID_SKIP] != (MIDI_NOTE | 36) ||
        cfg.midi_code[KEYID_VOL_DOWN] != (MIDI_CC_DOWN | 3) ||
        cfg.midi[KEYID_PAUSE][0] != '\0' || cfg.midi_code[KEYID_PAUSE] != -1 ||
        cfg.midi_code[KEYID_JOG_UP] != -1 || cfg.key_jog_up != 72 ||
        cfg.dial_modes[0] != DIAL_JOG || cfg.dial_modes[1] != DIAL_NONE ||
        cfg.dial_modes[2] != DIAL_NONE) {
        log_error("autodj", "MIDI config test did not load the bindings");
        goto out;
    }
    if (config_load_edit(path, &edit) != 0 || strcmp(edit.midi[KEYID_SKIP], "NOTE_36") != 0 ||
        strcmp(edit.midi[KEYID_PAUSE], "KEY_A") != 0 || strcmp(edit.dial_mode[0], "jog") != 0 ||
        strcmp(edit.dial_mode[1], "none") != 0 || strcmp(edit.dial_mode[2], "none") != 0 ||
        strcmp(edit.key_jog_up, "KEY_KP8") != 0) {
        log_error("autodj", "MIDI config test did not load the editor text");
        goto out;
    }
    snprintf(edit.dial_mode[2], sizeof edit.dial_mode[2], "spin");
    snprintf(edit.midi[KEYID_REVERB], sizeof edit.midi[KEYID_REVERB], "note_36");
    if (config_validate_edit(&edit, &cfg, err, sizeof err) == 0 ||
        strstr(err, "KEY_A") == NULL || strstr(err, "Dial mode 3 must be") == NULL ||
        strstr(err, "NOTE_36 is used for skip MIDI and reverb MIDI.") == NULL) {
        log_error("autodj", "MIDI config test missed a bad form: %s", err);
        goto out;
    }
    edit.midi[KEYID_PAUSE][0] = '\0';
    edit.midi[KEYID_REVERB][0] = '\0';
    snprintf(edit.dial_mode[2], sizeof edit.dial_mode[2], "volume");
    if (config_validate_edit(&edit, &cfg, err, sizeof err) != 0 ||
        cfg.midi_code[KEYID_VOL_UP] != (MIDI_CC_UP | 3) || cfg.dial_modes[2] != DIAL_VOLUME ||
        config_save(path, &cfg, err, sizeof err) != 0 || read_text(path, got, sizeof got) != 0 ||
        strstr(got, "skip_midi=NOTE_36\n") == NULL ||
        strstr(got, "volume_down_midi=CC_3_DOWN\n") == NULL ||
        strstr(got, "pause_resume_midi=\n") == NULL || strstr(got, "dial_default") != NULL ||
        strstr(got, "dial_mode_3=volume\n") == NULL || strstr(got, "reverb_midi") != NULL ||
        strstr(got, "jog_up=KEY_KP8\n") == NULL) {
        log_error("autodj", "MIDI config test saved unexpected lines: %s\n%s", err, got);
        goto out;
    }
    rc = 0;
out:
    remove(path);
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
    ConfigEdit edit;
    int slash = -1;
    int key_a = -1;
    int key_b = -1;
    int rc = 0;

    if (key_from_name("KEY_KPSLASH", &slash) != 0 ||
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
               cfg.loudness != 1 || cfg.loudness_lufs != -18 || cfg.dial_modes[0] != DIAL_VOLUME ||
               cfg.dial_modes[1] != DIAL_JOG || cfg.dial_modes[2] != DIAL_NONE ||
               cfg.key_vol_up != 78 || cfg.key_vol_down != -1 ||
               strcmp(cfg.kbd_vol_up, "=") != 0 || strcmp(cfg.kbd_jog_down, ",") != 0 ||
               cfg.key_pause != 82 || cfg.key_skip != 83 ||
               cfg.key_reverb != -1 || cfg.key_pitch != key_b || cfg.key_autopan != -1 ||
               cfg.key_phaser != key_a || cfg.fx.phaser_on != 0 ||
               cfg.fx.ps_pitch != -0.5f ||
               cfg.fx.rv_decay != 0.5f || cfg.fx.ps_semi != 1 || cfg.fx.ap_period != 1.5f ||
               cfg.fx.ap_smooth != 0.3f || cfg.fx.ap_distance != 75 ||
               cfg.fx.ph_stages != 7 || cfg.fx.ph_blend != 80 || cfg.fx.ph_low != 300 ||
               cfg.fx.ph_high != 3000 || cfg.fx.ph_period != 1 ||
               cfg.fx.chorus_on != 0 || cfg.fx.flanger_on != 0 ||
               cfg.fx.ch_delay != 20 || cfg.fx.ch_voices != 2 || cfg.fx.ch_depth != 5 ||
               cfg.fx.fl_delay != 1 || cfg.fx.fl_depth != 0.5f || cfg.fx.fl_feedback != 0.5f ||
               cfg.key_chorus != -1 || cfg.key_flanger != -1 ||
               cfg.key_effects_off != -1 ||
               strcmp(cfg.kbd_pause, "Space") != 0 || strcmp(cfg.kbd_skip, "Right") != 0 ||
               strcmp(cfg.kbd_prev, "Left") != 0 || strcmp(cfg.kbd_rew05, "1") != 0 ||
               strcmp(cfg.kbd_pitch, "w") != 0 || strcmp(cfg.kbd_effects_off, "Tab") != 0 ||
               cfg.fx.reverb_on != 0 || cfg.fx.pitch_on != 0 || cfg.fx.autopan_on != 0 ||
               cfg.key_prev != -1 || cfg.key_rew05 != -1 ||
               cfg.key_rew10 != -1 || cfg.key_rew15 != -1) {
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
            cfg.key_prev != key_a || cfg.key_rew05 != -1 ||
            cfg.key_rew10 != -1 || cfg.key_rew15 != -1) {
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
                        cfg.key_prev != -1 || cfg.key_rew05 != -1 ||
                        cfg.key_rew10 != -1 || cfg.key_rew15 != -1)) {
            log_error("autodj", "config test filled a missing keypad key");
            rc = -1;
        }
        fp = rc == 0 ? fopen(path3, "w") : NULL;
        if (fp) {
            fputs("[Main]\n"
                  "Folder=/tmp\n"
                  "[Keys]\n"
                  "rewind_1_5=\n"
                  "previous=\n",
                  fp);
            fclose(fp);
        }
        if (rc == 0 && (!fp || config_load(path3, &cfg) != 0 || cfg.key_prev != -1 ||
                        cfg.key_prev_name[0] != '\0' || cfg.key_rew15 != -1 ||
                        cfg.key_rew15_name[0] != '\0' || cfg.key_rew05 != -1 ||
                        cfg.key_rew10 != -1)) {
            log_error("autodj", "config test filled a key that was left blank");
            rc = -1;
        }
        if (rc == 0 && (config_load_edit(path3, &edit) != 0 ||
                        edit.key_prev[0] != '\0' || edit.key_rew15[0] != '\0' ||
                        edit.key_rew05[0] != '\0' || edit.key_rew10[0] != '\0' ||
                        strcmp(edit.kbd_pause, "Space") != 0)) {
            log_error("autodj", "config editor filled a keypad key that was left blank");
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
            cfg.dial_modes[0] != DIAL_JOG || cfg.dial_modes[1] != DIAL_VOLUME ||
            cfg.dial_modes[2] != DIAL_NONE) {
            log_error("autodj", "config test did not read the dial keys");
            rc = -1;
        } else if (config_save(path5, &cfg, err, sizeof err) != 0 ||
                   file_count(path5, "jog_forward") != 0 ||
                   file_count(path5, "jog_back") != 0 ||
                   file_count(path5, "dial_forward=REMOTE_DIAL_CW\n") != 1 ||
                   file_count(path5, "dial_back=REMOTE_DIAL_CCW\n") != 1 ||
                   file_count(path5, "dial_mode=REMOTE_CENTER\n") != 1 ||
                   file_count(path5, "dial_default") != 0 ||
                   file_count(path5, "dial_mode_1=jog\n") != 1 ||
                   file_count(path5, "dial_mode_2=volume\n") != 1 ||
                   file_count(path5, "dial_mode_3=none\n") != 1 ||
                   config_load(path5, &cfg) != 0 || cfg.key_dial_fwd != cw) {
            log_error("autodj", "config test did not rename the jog keys");
            rc = -1;
        }
        remove(path5);
    }
    if (rc == 0 && kbd_config_self_test() != 0) {
        rc = -1;
    }
    if (rc == 0 && midi_config_self_test() != 0) {
        rc = -1;
    }
    if (rc == 0 && edit_self_test() != 0) {
        rc = -1;
    }
    return rc;
}
