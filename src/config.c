#include "config.h"

#include "keys.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int config_self_test(void)
{
    char path[] = "/tmp/autodj-cfg-XXXXXX";
    int fd;
    FILE *fp;
    Config cfg;
    int rc = 0;

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
               cfg.key_vol_up != 78 || cfg.key_vol_dn != 74) {
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
    return rc;
}
