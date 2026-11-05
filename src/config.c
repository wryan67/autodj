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
    KEYID_UP,
    KEYID_DN,
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

static void set_key(Config *cfg, const char *action, const char *keyname)
{
    int code = -1;

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
    } else if (eq_ci(action, "volume_up") || eq_ci(action, "brake_delayvolume_up")) {
        /* brake_delayvolume_up is the mashed name from the sample config. */
        if (eq_ci(action, "brake_delayvolume_up")) {
            log_warn("autodj", "treating brake_delayvolume_up as volume_up");
        }
        cfg->key_vol_up = code;
        snprintf(cfg->key_vol_up_name, sizeof cfg->key_vol_up_name, "%s", keyname);
    } else if (eq_ci(action, "volume_dn") || eq_ci(action, "volume_down")) {
        cfg->key_vol_dn = code;
        snprintf(cfg->key_vol_dn_name, sizeof cfg->key_vol_dn_name, "%s", keyname);
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
           cfg->key_vol_up == code || cfg->key_vol_dn == code;
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
    cfg->key_pause = -1;
    cfg->key_skip = -1;
    cfg->key_prev = -1;
    cfg->key_rew05 = -1;
    cfg->key_rew10 = -1;
    cfg->key_rew15 = -1;
    cfg->key_vol_up = -1;
    cfg->key_vol_dn = -1;
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

static int is_volume_assignment(const char *line)
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
    return eq_ci(buf, "volume");
}

static void write_volume_line(FILE *out, const char *line, int percent)
{
    const char *eq = strchr(line, '=');
    const char *p;

    if (!eq) {
        fprintf(out, "volume=%d\n", percent);
        return;
    }
    fwrite(line, 1, (size_t)(eq - line) + 1, out);
    p = eq + 1;
    while (*p == ' ' || *p == '\t') {
        fputc(*p, out);
        p++;
    }
    fprintf(out, "%d\n", percent);
}

static int ends_with_newline(const char *text)
{
    size_t n = strlen(text);
    return n > 0 && text[n - 1] == '\n';
}

int config_save_volume(const char *path, int percent)
{
    FILE *in;
    FILE *out = NULL;
    struct CfgLine *head = NULL;
    struct CfgLine *tail = NULL;
    struct CfgLine *main_header = NULL;
    struct CfgLine *line;
    char buf[AUTODJ_PATH + 128];
    char tmp[AUTODJ_PATH + 16];
    char section[64];
    struct stat st;
    int fd = -1;
    int in_main = 0;
    int found = 0;
    int created = 0;
    int rc = -1;

    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
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
            in_main = eq_ci(section, "Main");
            if (in_main && !main_header) {
                main_header = line;
            }
            continue;
        }
        if (in_main && is_volume_assignment(line->text)) {
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

    in_main = 0;
    for (line = head; line; line = line->next) {
        if (!found && line == main_header) {
            fputs(line->text, out);
            if (!ends_with_newline(line->text)) {
                fputc('\n', out);
            }
            fprintf(out, "volume=%d\n", percent);
            continue;
        }
        if (section_name(line->text, section, sizeof section)) {
            in_main = eq_ci(section, "Main");
        }
        if (in_main && is_volume_assignment(line->text)) {
            write_volume_line(out, line->text, percent);
            continue;
        }
        fputs(line->text, out);
    }
    if (!found && !main_header) {
        if (tail && !ends_with_newline(tail->text)) {
            fputc('\n', out);
        }
        fprintf(out, "[Main]\nvolume=%d\n", percent);
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
    snprintf(edit->key_prev, sizeof edit->key_prev, "KEY_KPSLASH");
    snprintf(edit->key_rew05, sizeof edit->key_rew05, "KEY_KP1");
    snprintf(edit->key_rew10, sizeof edit->key_rew10, "KEY_KP2");
    snprintf(edit->key_rew15, sizeof edit->key_rew15, "KEY_KP3");
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
        } else if (eq_ci(key, "volume_up") || eq_ci(key, "brake_delayvolume_up")) {
            copy_raw(edit->key_vol_up, sizeof edit->key_vol_up, val);
        } else if (eq_ci(key, "volume_dn") || eq_ci(key, "volume_down")) {
            copy_raw(edit->key_vol_dn, sizeof edit->key_vol_dn, val);
        }
    }
}

/* Drop a keypad default when the file already gave that key to another action. */
static void release_taken_default(ConfigEdit *edit, char *field, const char *defname)
{
    char *fields[8];
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
    fields[6] = edit->key_vol_up;
    fields[7] = edit->key_vol_dn;
    for (i = 0; i < 8; i++) {
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
    trim(in.key_pause);
    trim(in.key_skip);
    trim(in.key_prev);
    trim(in.key_rew05);
    trim(in.key_rew10);
    trim(in.key_rew15);
    trim(in.key_vol_up);
    trim(in.key_vol_dn);

    memset(&out, 0, sizeof out);
    out.key_pause = -1;
    out.key_skip = -1;
    out.key_prev = -1;
    out.key_rew05 = -1;
    out.key_rew10 = -1;
    out.key_rew15 = -1;
    out.key_vol_up = -1;
    out.key_vol_dn = -1;
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
    key_ok[KEYID_UP] = take_key(err, err_n, in.key_vol_up, "Volume up",
                                &out.key_vol_up, out.key_vol_up_name,
                                sizeof out.key_vol_up_name) == 0;
    key_ok[KEYID_DN] = take_key(err, err_n, in.key_vol_dn, "Volume down",
                                &out.key_vol_dn, out.key_vol_dn_name,
                                sizeof out.key_vol_dn_name) == 0;
    keys[KEYID_PAUSE] = in.key_pause;
    keys[KEYID_SKIP] = in.key_skip;
    keys[KEYID_PREV] = in.key_prev;
    keys[KEYID_REW05] = in.key_rew05;
    keys[KEYID_REW10] = in.key_rew10;
    keys[KEYID_REW15] = in.key_rew15;
    keys[KEYID_UP] = in.key_vol_up;
    keys[KEYID_DN] = in.key_vol_dn;
    labels[KEYID_PAUSE] = "pause/resume";
    labels[KEYID_SKIP] = "skip";
    labels[KEYID_PREV] = "previous";
    labels[KEYID_REW05] = "rewind 0.5s";
    labels[KEYID_REW10] = "rewind 1s";
    labels[KEYID_REW15] = "rewind 1.5s";
    labels[KEYID_UP] = "volume up";
    labels[KEYID_DN] = "volume down";
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
    if (eq_ci(key, "volume_up") || eq_ci(key, "brake_delayvolume_up")) {
        return KEYID_UP;
    }
    if (eq_ci(key, "volume_dn") || eq_ci(key, "volume_down")) {
        return KEYID_DN;
    }
    return -1;
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
    default:
        snprintf(dest, n, "%d", cfg->loudness_lufs);
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
    case KEYID_UP:
        s = cfg->key_vol_up_name;
        break;
    case KEYID_DN:
        s = cfg->key_vol_dn_name;
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

int config_save(const char *path, const Config *cfg, char *err, size_t err_n)
{
    static const char *main_names[] = {
        "Folder", "shuffle", "overlap", "trim_silence", "pause_time",
        "resume_time", "volume", "loudness", "loudness_lufs"
    };
    static const char *key_names[] = {
        "pause_resume", "skip", "previous", "rewind_0_5", "rewind_1",
        "rewind_1_5", "volume_up", "volume_dn"
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
    char buf[AUTODJ_PATH + 128];
    char tmp[AUTODJ_PATH + 16];
    char section[64];
    char value[AUTODJ_PATH];
    struct stat st;
    int fd = -1;
    int created = 0;
    int in_main = 0;
    int in_keys = 0;
    int main_seen[9] = {0};
    int keys_seen[KEYID_COUNT] = {0};
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
            prev = line;
            line = next;
            continue;
        }
        if ((in_main || in_keys) && line_setting_key(line->text, key, sizeof key)) {
            int is_main = in_main && (id = main_id(key)) >= 0;
            int is_key = in_keys && (id = keys_id(key)) >= 0;

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

    if (ensure_section(&head, "Main", &main_header, &main_last) != 0) {
        set_err(err, err_n, "out of memory updating %s", path);
        goto done;
    }
    for (id = 0; id < 9; id++) {
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

    id = 0;
    for (id = 0; id < KEYID_COUNT; id++) {
        key_value(cfg, id, value, sizeof value);
        if (!keys_seen[id] && value[0]) {
            break;
        }
    }
    if (id < KEYID_COUNT) {
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
        "custom=KEY_A\n";
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
        "\n"
        "[Keys]\n"
        "# key comment\n"
        "skip=KEY_KPDOT\n"
        "volume_down=KEY_KPMINUS\n"
        "custom=KEY_A\n"
        "pause_resume=KEY_KP0\n";
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
        strcmp(edit.key_vol_dn, "KEY_KPMINUS") != 0 || edit.key_pause[0] != '\0') {
        log_error("autodj", "config editor test loaded unexpected text");
        rc = -1;
    }
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.folder, sizeof cfg.folder, "/tmp");
    cfg.shuffle = 1;
    cfg.overlap_sec = 8;
    cfg.trim_silence = 1;
    cfg.pause_sec = 3;
    cfg.resume_sec = 4;
    cfg.volume = 40;
    cfg.loudness = 1;
    cfg.loudness_lufs = -18;
    snprintf(cfg.key_pause_name, sizeof cfg.key_pause_name, "KEY_KP0");
    snprintf(cfg.key_skip_name, sizeof cfg.key_skip_name, "KEY_KPDOT");
    snprintf(cfg.key_vol_dn_name, sizeof cfg.key_vol_dn_name, "KEY_KPMINUS");
    if (rc == 0 && (config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 || strcmp(got, expect) != 0)) {
        log_error("autodj", "config editor test did not keep the file layout: %s\n%s", err, got);
        rc = -1;
    }
    cfg.key_vol_dn_name[0] = '\0';
    if (rc == 0 && (config_save(path, &cfg, err, sizeof err) != 0 ||
                    read_text(path, got, sizeof got) != 0 ||
                    strstr(got, "volume_down=") != NULL ||
                    strstr(got, "pause_resume=KEY_KP0\n") == NULL ||
                    strstr(got, "custom=KEY_A\n") == NULL)) {
        log_error("autodj", "config editor test did not clear a key\n%s", got);
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
                    strcmp(edit.key_rew15, "KEY_KP3") != 0)) {
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
    snprintf(edit.key_pause, sizeof edit.key_pause, "KEY_KP0");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KPDOT");
    if (rc == 0 && (config_validate_edit(&edit, &cfg, err, sizeof err) != 0 ||
                    strcmp(cfg.folder, "/tmp") != 0 || cfg.volume != 40 ||
                    cfg.key_pause != 82 || cfg.key_vol_up != -1 ||
                    cfg.key_prev != -1 || cfg.key_rew05 != -1 ||
                    cfg.key_rew10 != -1 || cfg.key_rew15 != -1)) {
        log_error("autodj", "config editor test rejected a valid form: %s", err);
        rc = -1;
    }
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
    int rc = 0;

    if (key_from_name("KEY_KPSLASH", &slash) != 0 || key_from_name("KEY_KP1", &kp1) != 0 ||
        key_from_name("KEY_KP2", &kp2) != 0 || key_from_name("KEY_KP3", &kp3) != 0 ||
        key_from_name("KEY_A", &key_a) != 0) {
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
          "volume_dn=KEY_KPMINUS\n",
          fp);
    fclose(fp);

    if (config_load(path, &cfg) != 0) {
        rc = -1;
    } else if (strcmp(cfg.folder, "/tmp/music folder") != 0 || cfg.shuffle != 0 ||
               cfg.overlap_sec != 3 || cfg.trim_silence != 1 || cfg.pause_sec != 2 ||
               cfg.resume_sec != 4 || cfg.volume != 100 || cfg.volume_set != 0 ||
               cfg.loudness != 1 || cfg.loudness_lufs != -18 ||
               cfg.key_pause != 82 || cfg.key_skip != 83 ||
               cfg.key_vol_up != 78 || cfg.key_vol_dn != 74 ||
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
               strcmp(cfg.folder, "/tmp/music folder") != 0 || cfg.key_vol_up != 78) {
        log_error("autodj", "config test did not reload stored volume");
        rc = -1;
    } else if (config_save_volume(path, 0) != 0 || file_count(path, "volume=") != 1 ||
               config_load(path, &cfg) != 0 || cfg.volume != 0) {
        log_error("autodj", "config test did not store a zero volume");
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
    if (rc == 0 && edit_self_test() != 0) {
        rc = -1;
    }
    return rc;
}
