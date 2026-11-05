#include "cfgedit.h"

#include "config.h"
#include "keys.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

/*
 * Full-screen editor for master.cfg. Tab and the arrow keys move between
 * fields. Left and right move the cursor in a text field, and step to the
 * next field at either end. A key binding is captured from the keypad, or
 * from another keyboard if the press is not on the keypad. Esc cancels.
 * Save validates before it writes.
 */

enum {
    FE_FOLDER = 0,
    FE_SHUFFLE,
    FE_OVERLAP,
    FE_TRIM,
    FE_PAUSE,
    FE_RESUME,
    FE_VOLUME,
    FE_LOUDNESS,
    FE_LUFS,
    FE_KEY_PAUSE,
    FE_KEY_SKIP,
    FE_KEY_UP,
    FE_KEY_DN,
    FE_FIELD_COUNT,
    FE_SAVE = FE_FIELD_COUNT,
    FE_CANCEL,
    FE_COUNT
};

enum {
    LABEL_W = 16,
    NUM_W = 8,
    FRAME_H = 20,
    ROW_FIELD0 = 3,
    ROW_BUTTONS = 17,
    MIN_ROWS = 22,
    MIN_COLS = 68,
    SAVE_W = 8,
    CANCEL_W = 10
};

enum {
    EV_NONE = 0,
    EV_CHAR,
    EV_UP,
    EV_DOWN,
    EV_LEFT,
    EV_RIGHT,
    EV_HOME,
    EV_END,
    EV_BACKSPACE,
    EV_DELETE,
    EV_TAB,
    EV_STAB,
    EV_ENTER,
    EV_ESC,
    EV_MOUSE,
    EV_RESIZE,
    EV_QUIT,
    EV_HWKEY
};

struct Event {
    int kind;
    char text[8];
    int x;
    int y;
    int code;
};

struct Ui {
    int rows;
    int cols;
    int too_small;
    int frame_r;
    int frame_c;
    int frame_w;
    int frame_h;
    int label_c;
    int box_c;
    int field_r[FE_FIELD_COUNT];
    int box_w[FE_FIELD_COUNT];
    int save_r;
    int save_c;
    int cancel_r;
    int cancel_c;
    int hint_r;
    int ok_on;
    int ok_r;
    int ok_c;
    int ok_w;
};

struct Editor {
    const char *path;
    ConfigEdit edit;
    struct Ui ui;
    int focus;
    int cursor;
    int dialog;
    int capture;
    int capture_id;
    int saved;
    char dialog_title[64];
    char err[1024];
};

static int g_tty = -1;
static int g_raw;
static int g_screen;
static int g_unget = -1;
static struct termios g_orig;
static volatile sig_atomic_t g_winch;
static struct sigaction g_old_winch;
static struct sigaction g_old_term;
static struct sigaction g_old_hup;
static struct sigaction g_old_int;
static int g_key_fds[32];
static int g_key_n;
static int g_keypad;
static int g_keypad_grabbed;

static const char *field_label(int id)
{
    static const char *labels[FE_FIELD_COUNT] = {
        "Folder",
        "Shuffle",
        "Overlap sec",
        "Trim silence",
        "Pause sec",
        "Resume sec",
        "Volume %",
        "Loudness",
        "Target LUFS",
        "Pause/resume",
        "Skip",
        "Volume up",
        "Volume down"
    };

    if (id < 0 || id >= FE_FIELD_COUNT) {
        return "";
    }
    return labels[id];
}

static char *field_text(ConfigEdit *edit, int id, size_t *cap)
{
    switch (id) {
    case FE_FOLDER:
        *cap = sizeof edit->folder;
        return edit->folder;
    case FE_SHUFFLE:
        *cap = sizeof edit->shuffle;
        return edit->shuffle;
    case FE_OVERLAP:
        *cap = sizeof edit->overlap;
        return edit->overlap;
    case FE_TRIM:
        *cap = sizeof edit->trim_silence;
        return edit->trim_silence;
    case FE_PAUSE:
        *cap = sizeof edit->pause_time;
        return edit->pause_time;
    case FE_RESUME:
        *cap = sizeof edit->resume_time;
        return edit->resume_time;
    case FE_VOLUME:
        *cap = sizeof edit->volume;
        return edit->volume;
    case FE_LOUDNESS:
        *cap = sizeof edit->loudness;
        return edit->loudness;
    case FE_LUFS:
        *cap = sizeof edit->loudness_lufs;
        return edit->loudness_lufs;
    case FE_KEY_PAUSE:
        *cap = sizeof edit->key_pause;
        return edit->key_pause;
    case FE_KEY_SKIP:
        *cap = sizeof edit->key_skip;
        return edit->key_skip;
    case FE_KEY_UP:
        *cap = sizeof edit->key_vol_up;
        return edit->key_vol_up;
    case FE_KEY_DN:
        *cap = sizeof edit->key_vol_dn;
        return edit->key_vol_dn;
    default:
        *cap = 0;
        return NULL;
    }
}

static int is_number_field(int id)
{
    return id == FE_OVERLAP || id == FE_PAUSE || id == FE_RESUME ||
           id == FE_VOLUME || id == FE_LUFS;
}

static int is_key_field(int id)
{
    return id == FE_KEY_PAUSE || id == FE_KEY_SKIP || id == FE_KEY_UP ||
           id == FE_KEY_DN;
}

static int is_check_field(const ConfigEdit *edit, int id)
{
    size_t cap;
    int on = 0;
    char *text;

    if (id != FE_SHUFFLE && id != FE_TRIM && id != FE_LOUDNESS) {
        return 0;
    }
    text = field_text((ConfigEdit *)edit, id, &cap);
    return text && config_parse_bool(text, &on) == 0;
}

static int utf8_need(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xe0) == 0xc0) {
        return 2;
    }
    if ((c & 0xf0) == 0xe0) {
        return 3;
    }
    if ((c & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

static int utf8_span(const unsigned char *s)
{
    int need;
    int i;

    if (*s == 0) {
        return 0;
    }
    need = utf8_need(*s);
    for (i = 1; i < need; i++) {
        if (s[i] == 0) {
            return 1;
        }
    }
    return need;
}

static int utf8_cols(const char *text, int byte_end)
{
    int i = 0;
    int cols = 0;

    if (byte_end < 0) {
        byte_end = 0;
    }
    while (text[i] && i < byte_end) {
        i += utf8_span((const unsigned char *)text + i);
        cols++;
    }
    return cols;
}

static int byte_at_column(const char *text, int col)
{
    int i = 0;
    int c = 0;

    if (col <= 0) {
        return 0;
    }
    while (text[i] && c < col) {
        i += utf8_span((const unsigned char *)text + i);
        c++;
    }
    return i;
}

static int utf8_prev(const char *text, int pos)
{
    if (pos <= 0) {
        return 0;
    }
    pos--;
    while (pos > 0 && ((unsigned char)text[pos] & 0xc0) == 0x80) {
        pos--;
    }
    return pos;
}

static void text_insert(char *buf, size_t cap, int *cursor, const char *add)
{
    size_t len = strlen(buf);
    size_t n = strlen(add);
    size_t at = (size_t)*cursor;

    if (at > len) {
        at = len;
    }
    if (n == 0 || len + n >= cap) {
        return;
    }
    memmove(buf + at + n, buf + at, len - at + 1);
    memcpy(buf + at, add, n);
    *cursor = (int)(at + n);
}

static void text_backspace(char *buf, int *cursor)
{
    int at = *cursor;
    int prev;
    size_t len;

    if (at <= 0) {
        return;
    }
    prev = utf8_prev(buf, at);
    len = strlen(buf);
    memmove(buf + prev, buf + at, len - (size_t)at + 1);
    *cursor = prev;
}

static void text_delete(char *buf, int *cursor)
{
    int at = *cursor;
    int next;
    size_t len;

    if (buf[at] == '\0') {
        return;
    }
    next = at + utf8_span((const unsigned char *)buf + at);
    len = strlen(buf);
    memmove(buf + at, buf + next, len - (size_t)next + 1);
}

static void toggle_check(struct Editor *ed, int id)
{
    size_t cap;
    char *text = field_text(&ed->edit, id, &cap);
    int on = 0;

    if (!text || config_parse_bool(text, &on) != 0) {
        return;
    }
    snprintf(text, cap, "%s", on ? "false" : "true");
}

static void move_focus(struct Editor *ed, int delta)
{
    int id = ed->focus + delta;

    if (id < 0) {
        id = FE_COUNT - 1;
    }
    if (id >= FE_COUNT) {
        id = 0;
    }
    ed->focus = id;
    if (id < FE_FIELD_COUNT) {
        size_t cap;
        char *text = field_text(&ed->edit, id, &cap);

        ed->cursor = text ? (int)strlen(text) : 0;
    }
}

static int field_scroll(const char *text, int cursor, int interior)
{
    int col;

    if (interior < 1) {
        return 0;
    }
    col = utf8_cols(text, cursor);
    if (col < interior) {
        return 0;
    }
    return col - interior + 1;
}

static void layout_ui(struct Ui *ui, const ConfigEdit *edit, int rows, int cols)
{
    int avail;
    int i;
    int total;
    int start;

    memset(ui, 0, sizeof *ui);
    ui->rows = rows;
    ui->cols = cols;
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        ui->too_small = 1;
        return;
    }
    ui->frame_w = cols - 2;
    if (ui->frame_w > 96) {
        ui->frame_w = 96;
    }
    ui->frame_h = FRAME_H;
    ui->frame_r = (rows - FRAME_H - 1) / 2 + 1;
    if (ui->frame_r < 1) {
        ui->frame_r = 1;
    }
    ui->frame_c = (cols - ui->frame_w) / 2 + 1;
    ui->label_c = ui->frame_c + 2;
    ui->box_c = ui->label_c + LABEL_W + 1;
    avail = ui->frame_w - 4 - LABEL_W - 1;
    if (avail < NUM_W) {
        ui->too_small = 1;
        return;
    }
    for (i = 0; i < FE_FIELD_COUNT; i++) {
        ui->field_r[i] = ui->frame_r + ROW_FIELD0 + i;
        if (is_check_field(edit, i)) {
            ui->box_w[i] = 3;
        } else if (is_number_field(i)) {
            ui->box_w[i] = NUM_W;
        } else {
            ui->box_w[i] = avail;
        }
    }
    ui->save_r = ui->frame_r + ROW_BUTTONS;
    ui->cancel_r = ui->save_r;
    total = SAVE_W + 4 + CANCEL_W;
    start = ui->frame_c + (ui->frame_w - total) / 2;
    ui->save_c = start;
    ui->cancel_c = start + SAVE_W + 4;
    ui->hint_r = ui->frame_r + ui->frame_h;
}

static void cup(FILE *fp, int row, int col)
{
    fprintf(fp, "\033[%d;%dH", row, col);
}

static void draw_hbar(FILE *fp, int row, int col, int width, const char *title)
{
    char bar[128];
    int n = width;
    int tlen;

    if (n < 2) {
        return;
    }
    if (n > 120) {
        n = 120;
    }
    memset(bar, '-', (size_t)n);
    bar[0] = '+';
    bar[n - 1] = '+';
    if (title) {
        tlen = (int)strlen(title);
        if (tlen > n - 2) {
            tlen = n - 2;
        }
        memcpy(bar + 2, title, (size_t)tlen);
    }
    bar[n] = '\0';
    cup(fp, row, col);
    fputs(bar, fp);
}

static void shorten_path(const char *path, int width, char *dest, size_t dest_n)
{
    size_t n = strlen(path);

    if (width < 1 || dest_n == 0) {
        if (dest_n) {
            dest[0] = '\0';
        }
        return;
    }
    if ((int)n <= width) {
        snprintf(dest, dest_n, "%s", path);
        return;
    }
    if (width <= 3) {
        snprintf(dest, dest_n, "%.*s", width, path);
        return;
    }
    snprintf(dest, dest_n, "...%s", path + n - (size_t)(width - 3));
}

static void draw_text_box(FILE *fp, int row, int col, int width, const char *text,
                          int cursor, int focused, int caret)
{
    int interior = width - 2;
    int scroll;
    int byte;
    int i;
    int cur_col;

    if (interior < 1) {
        interior = 1;
    }
    if (cursor < 0) {
        cursor = 0;
    }
    if ((size_t)cursor > strlen(text)) {
        cursor = (int)strlen(text);
    }
    scroll = focused ? field_scroll(text, cursor, interior) : 0;
    byte = byte_at_column(text, scroll);
    cur_col = utf8_cols(text, cursor);
    cup(fp, row, col);
    if (focused) {
        fputs("\033[7m", fp);
    }
    fputc('[', fp);
    for (i = 0; i < interior; i++) {
        char cp[8];
        int n = 0;
        int at_cursor = caret && focused && scroll + i == cur_col;

        if (text[byte] == '\0') {
            cp[0] = ' ';
            cp[1] = '\0';
        } else {
            n = utf8_span((const unsigned char *)text + byte);
            memcpy(cp, text + byte, (size_t)n);
            cp[n] = '\0';
            byte += n;
        }
        if (at_cursor) {
            fputs("\033[0m", fp);
            fputs(cp, fp);
            fputs("\033[7m", fp);
        } else {
            fputs(cp, fp);
        }
    }
    fputc(']', fp);
    if (focused) {
        fputs("\033[0m", fp);
    }
}

static void draw_check(FILE *fp, int row, int col, int on, int focused)
{
    cup(fp, row, col);
    if (focused) {
        fputs("\033[7m", fp);
    }
    fputs(on ? "[x]" : "[ ]", fp);
    if (focused) {
        fputs("\033[0m", fp);
    }
}

static void draw_button(FILE *fp, int row, int col, const char *text, int focused)
{
    cup(fp, row, col);
    if (focused) {
        fputs("\033[7m", fp);
    }
    fputs(text, fp);
    if (focused) {
        fputs("\033[0m", fp);
    }
}

static int wrap_lines(const char *msg, int width, char lines[][96], int max_lines)
{
    int n = 0;

    if (width < 8) {
        width = 8;
    }
    if (width > 90) {
        width = 90;
    }
    while (*msg && n < max_lines) {
        const char *nl = strchr(msg, '\n');
        int seglen = nl ? (int)(nl - msg) : (int)strlen(msg);

        if (seglen == 0) {
            if (!nl) {
                break;
            }
            msg = nl + 1;
            continue;
        }
        while (seglen > 0 && n < max_lines) {
            int take = seglen < width ? seglen : width;

            if (seglen > width) {
                int b = take;

                while (b > 8 && msg[b] != ' ') {
                    b--;
                }
                if (msg[b] == ' ') {
                    take = b;
                }
            }
            if (take < 1) {
                take = 1;
            }
            memcpy(lines[n], msg, (size_t)take);
            lines[n][take] = '\0';
            n++;
            msg += take;
            seglen -= take;
            while (seglen > 0 && *msg == ' ') {
                msg++;
                seglen--;
            }
        }
        if (nl) {
            msg = nl + 1;
        } else {
            break;
        }
    }
    return n;
}

static void draw_dialog(FILE *fp, struct Ui *ui, const char *title, const char *msg,
                        const char *button)
{
    char lines[16][96];
    int max_lines = ui->rows - 8;
    int text_w = ui->cols - 8;
    int nlines;
    int box_w;
    int box_h;
    int box_r;
    int box_c;
    int r;
    int i;

    if (max_lines < 1) {
        max_lines = 1;
    }
    if (max_lines > 16) {
        max_lines = 16;
    }
    if (text_w > 56) {
        text_w = 56;
    }
    if (text_w < 20) {
        text_w = 20;
    }
    if (!title || !title[0]) {
        title = "Cannot save";
    }
    if (!button || !button[0]) {
        button = "[ OK ]";
    }
    if (!msg || !msg[0]) {
        msg = "Cannot save.";
    }
    nlines = wrap_lines(msg, text_w, lines, max_lines);
    if (nlines < 1) {
        snprintf(lines[0], sizeof lines[0], "Cannot save.");
        nlines = 1;
    }
    box_w = text_w + 4;
    box_h = nlines + 6;
    if (box_h > ui->rows) {
        box_h = ui->rows;
    }
    if (box_w > ui->cols) {
        box_w = ui->cols;
    }
    box_r = (ui->rows - box_h) / 2 + 1;
    box_c = (ui->cols - box_w) / 2 + 1;
    if (box_r < 1) {
        box_r = 1;
    }
    if (box_c < 1) {
        box_c = 1;
    }
    for (r = 0; r < box_h; r++) {
        cup(fp, box_r + r, box_c);
        for (i = 0; i < box_w; i++) {
            fputc(' ', fp);
        }
    }
    draw_hbar(fp, box_r, box_c, box_w, NULL);
    draw_hbar(fp, box_r + box_h - 1, box_c, box_w, NULL);
    for (r = 1; r < box_h - 1; r++) {
        cup(fp, box_r + r, box_c);
        fputc('|', fp);
        cup(fp, box_r + r, box_c + box_w - 1);
        fputc('|', fp);
    }
    cup(fp, box_r + 1, box_c + 2);
    fputs("\033[1m", fp);
    fputs(title, fp);
    fputs("\033[0m", fp);
    for (i = 0; i < nlines; i++) {
        cup(fp, box_r + 3 + i, box_c + 2);
        fputs(lines[i], fp);
    }
    ui->ok_on = 1;
    ui->ok_w = (int)strlen(button);
    ui->ok_r = box_r + nlines + 4;
    ui->ok_c = box_c + (box_w - ui->ok_w) / 2;
    if (ui->ok_c < box_c + 1) {
        ui->ok_c = box_c + 1;
    }
    draw_button(fp, ui->ok_r, ui->ok_c, button, 1);
}

static void draw_screen(struct Editor *ed, FILE *fp)
{
    struct Ui *ui = &ed->ui;
    int i;
    const char *hint = "Tab moves   Space toggles   Enter sets a key   Esc cancels";
    int hint_len = (int)strlen(hint);
    int hint_c;

    fputs("\033[H\033[J\033[0m", fp);
    if (ui->too_small) {
        cup(fp, 1, 1);
        fputs("Terminal is too small for the config editor.", fp);
        cup(fp, 2, 1);
        fputs("Resize the window, or press Esc to cancel.", fp);
        return;
    }
    draw_hbar(fp, ui->frame_r, ui->frame_c, ui->frame_w, " autodj configuration ");
    draw_hbar(fp, ui->frame_r + ui->frame_h - 1, ui->frame_c, ui->frame_w, NULL);
    for (i = 1; i < ui->frame_h - 1; i++) {
        cup(fp, ui->frame_r + i, ui->frame_c);
        fputc('|', fp);
        cup(fp, ui->frame_r + i, ui->frame_c + ui->frame_w - 1);
        fputc('|', fp);
    }
    {
        char shown[AUTODJ_PATH];
        int width = ui->frame_w - 4;

        shorten_path(ed->path, width, shown, sizeof shown);
        cup(fp, ui->frame_r + 1, ui->label_c);
        fputs(shown, fp);
    }
    for (i = 0; i < FE_FIELD_COUNT; i++) {
        size_t cap;
        char *text = field_text(&ed->edit, i, &cap);
        int focused = ed->focus == i;
        int on = 0;

        cup(fp, ui->field_r[i], ui->label_c);
        if (focused) {
            fputs("\033[1m", fp);
        }
        fprintf(fp, "%-*s", LABEL_W, field_label(i));
        fputs("\033[0m", fp);
        if (is_check_field(&ed->edit, i)) {
            config_parse_bool(text, &on);
            draw_check(fp, ui->field_r[i], ui->box_c, on, focused);
        } else if (is_key_field(i)) {
            draw_text_box(fp, ui->field_r[i], ui->box_c, ui->box_w[i],
                          text ? text : "", 0, focused, 0);
        } else {
            int cursor = focused ? ed->cursor : 0;

            draw_text_box(fp, ui->field_r[i], ui->box_c, ui->box_w[i],
                          text ? text : "", cursor, focused, 1);
        }
    }
    draw_button(fp, ui->save_r, ui->save_c, "[ Save ]", ed->focus == FE_SAVE);
    draw_button(fp, ui->cancel_r, ui->cancel_c, "[ Cancel ]", ed->focus == FE_CANCEL);
    hint_c = ui->frame_c + (ui->frame_w - hint_len) / 2;
    if (hint_c < 1) {
        hint_c = 1;
    }
    if (ui->hint_r <= ui->rows) {
        cup(fp, ui->hint_r, hint_c);
        fputs(hint, fp);
    }
    if (ed->capture) {
        draw_dialog(fp, ui, "Select a key",
                    ed->err[0] ? ed->err : "Press a new key.", "[ Cancel ]");
    } else if (ed->dialog) {
        draw_dialog(fp, ui, ed->dialog_title, ed->err, "[ OK ]");
    }
}

static int tty_size(int fd, int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_row < 1 || ws.ws_col < 1) {
        *rows = 24;
        *cols = 80;
        return -1;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

static int render(struct Editor *ed)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *fp;
    int rows = 24;
    int cols = 80;
    int rc;

    tty_size(g_tty, &rows, &cols);
    layout_ui(&ed->ui, &ed->edit, rows, cols);
    fp = open_memstream(&buf, &n);
    if (!fp) {
        return -1;
    }
    draw_screen(ed, fp);
    if (fclose(fp) != 0) {
        free(buf);
        return -1;
    }
    rc = 0;
    {
        size_t off = 0;

        while (off < n) {
            ssize_t w = write(g_tty, buf + off, n - off);

            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
    }
    free(buf);
    return rc;
}

static void on_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}

static void restore_tty(void)
{
    static const char leave[] = "\033[?1000l\033[?1006l\033[?25h\033[?1049l";

    if (g_tty >= 0 && g_raw) {
        tcsetattr(g_tty, TCSAFLUSH, &g_orig);
        g_raw = 0;
    }
    if (g_tty >= 0 && g_screen) {
        ssize_t w = write(g_tty, leave, sizeof leave - 1);

        (void)w;
        g_screen = 0;
    }
}

static void on_fatal(int sig)
{
    (void)sig;
    restore_tty();
    _exit(1);
}

static int enter_tty(void)
{
    struct termios raw;
    struct sigaction sa;
    static const char enter[] = "\033[?1049h\033[?1006h\033[?1000h\033[?25l";

    g_tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (g_tty < 0) {
        log_error("autodj", "config editor needs a terminal");
        return -1;
    }
    if (tcgetattr(g_tty, &g_orig) != 0) {
        log_error("autodj", "cannot set the terminal: %s", strerror(errno));
        close(g_tty);
        g_tty = -1;
        return -1;
    }
    raw = g_orig;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(g_tty, TCSAFLUSH, &raw) != 0) {
        log_error("autodj", "cannot set the terminal: %s", strerror(errno));
        close(g_tty);
        g_tty = -1;
        return -1;
    }
    g_raw = 1;
    if (write(g_tty, enter, sizeof enter - 1) < 0) {
        restore_tty();
        close(g_tty);
        g_tty = -1;
        log_error("autodj", "cannot open the config screen");
        return -1;
    }
    g_screen = 1;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, &g_old_winch);
    sa.sa_handler = on_fatal;
    sigaction(SIGTERM, &sa, &g_old_term);
    sigaction(SIGHUP, &sa, &g_old_hup);
    sigaction(SIGINT, &sa, &g_old_int);
    return 0;
}

static void release_keys(void)
{
    if (g_keypad_grabbed && g_key_n > 0) {
        keys_ungrab(g_key_fds[0]);
    }
    g_keypad_grabbed = 0;
    g_keypad = 0;
    keys_close_capture(g_key_fds, g_key_n);
    g_key_n = 0;
}

static void leave_tty(void)
{
    release_keys();
    restore_tty();
    if (g_tty >= 0) {
        sigaction(SIGWINCH, &g_old_winch, NULL);
        sigaction(SIGTERM, &g_old_term, NULL);
        sigaction(SIGHUP, &g_old_hup, NULL);
        sigaction(SIGINT, &g_old_int, NULL);
        close(g_tty);
        g_tty = -1;
    }
}

static int read_byte(int fd, int timeout_ms, unsigned char *out)
{
    struct pollfd pfd;
    unsigned char b;

    if (g_unget >= 0) {
        *out = (unsigned char)g_unget;
        g_unget = -1;
        return 1;
    }
    for (;;) {
        int pr;
        ssize_t n;

        if (g_winch) {
            return 0;
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            return 0;
        }
        n = read(fd, &b, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        *out = b;
        return 1;
    }
}

static int read_until_final(int fd, char *buf, size_t buflen, unsigned char *final)
{
    size_t n = 0;

    while (n + 1 < buflen) {
        unsigned char c;
        int rc = read_byte(fd, 80, &c);

        if (rc != 1) {
            return -1;
        }
        if (c >= 0x40 && c <= 0x7e) {
            *final = c;
            buf[n] = '\0';
            return 0;
        }
        buf[n++] = (char)c;
    }
    return -1;
}

static int csi_param(const char *buf)
{
    int v = 0;

    if (!isdigit((unsigned char)buf[0])) {
        return 0;
    }
    while (isdigit((unsigned char)*buf)) {
        v = v * 10 + (*buf - '0');
        buf++;
    }
    return v;
}

static int event_from_byte(struct Event *ev, unsigned char c)
{
    int rc;

    memset(ev, 0, sizeof *ev);
    if (c == 3) {
        ev->kind = EV_QUIT;
        return 0;
    }
    if (c == '\t') {
        ev->kind = EV_TAB;
        return 0;
    }
    if (c == '\r' || c == '\n') {
        ev->kind = EV_ENTER;
        return 0;
    }
    if (c == 127 || c == 8) {
        ev->kind = EV_BACKSPACE;
        return 0;
    }
    if (c == 0x1b) {
        unsigned char n1;
        char seq[64];
        unsigned char final = 0;

        rc = read_byte(g_tty, 80, &n1);
        if (rc == 0) {
            ev->kind = g_winch ? EV_RESIZE : EV_ESC;
            if (g_winch) {
                g_winch = 0;
            }
            return 0;
        }
        if (rc != 1) {
            ev->kind = EV_ESC;
            return 0;
        }
        if (n1 == 'O') {
            unsigned char n2;

            if (read_byte(g_tty, 80, &n2) != 1) {
                ev->kind = EV_NONE;
                return 0;
            }
            if (n2 == 'A') {
                ev->kind = EV_UP;
            } else if (n2 == 'B') {
                ev->kind = EV_DOWN;
            } else if (n2 == 'C') {
                ev->kind = EV_RIGHT;
            } else if (n2 == 'D') {
                ev->kind = EV_LEFT;
            } else if (n2 == 'H') {
                ev->kind = EV_HOME;
            } else if (n2 == 'F') {
                ev->kind = EV_END;
            } else if (n2 == 'M') {
                ev->kind = EV_ENTER;
            } else {
                ev->kind = EV_NONE;
            }
            return 0;
        }
        if (n1 != '[') {
            g_unget = n1;
            ev->kind = EV_ESC;
            return 0;
        }
        if (read_until_final(g_tty, seq, sizeof seq, &final) != 0) {
            ev->kind = g_winch ? EV_RESIZE : EV_NONE;
            g_winch = 0;
            return 0;
        }
        if (seq[0] == '<') {
            int btn = 0;
            int x = 0;
            int y = 0;

            if (sscanf(seq, "<%d;%d;%d", &btn, &x, &y) == 3 &&
                final == 'M' && btn == 0 && x > 0 && y > 0) {
                ev->kind = EV_MOUSE;
                ev->x = x;
                ev->y = y;
            } else {
                ev->kind = EV_NONE;
            }
            return 0;
        }
        if (final == 'A') {
            ev->kind = EV_UP;
        } else if (final == 'B') {
            ev->kind = EV_DOWN;
        } else if (final == 'C') {
            ev->kind = EV_RIGHT;
        } else if (final == 'D') {
            ev->kind = EV_LEFT;
        } else if (final == 'H') {
            ev->kind = EV_HOME;
        } else if (final == 'F') {
            ev->kind = EV_END;
        } else if (final == 'Z') {
            ev->kind = EV_STAB;
        } else if (final == '~') {
            int p = csi_param(seq);

            if (p == 3) {
                ev->kind = EV_DELETE;
            } else if (p == 1 || p == 7) {
                ev->kind = EV_HOME;
            } else if (p == 4 || p == 8) {
                ev->kind = EV_END;
            } else {
                ev->kind = EV_NONE;
            }
        } else {
            ev->kind = EV_NONE;
        }
        return 0;
    }
    if (c < 32) {
        ev->kind = EV_NONE;
        return 0;
    }
    ev->text[0] = (char)c;
    if (c >= 0x80) {
        int need = utf8_need(c);
        int i;

        for (i = 1; i < need; i++) {
            unsigned char n;

            if (read_byte(g_tty, 20, &n) != 1) {
                break;
            }
            ev->text[i] = (char)n;
        }
        ev->text[i] = '\0';
    } else {
        ev->text[1] = '\0';
    }
    ev->kind = EV_CHAR;
    return 0;
}

static void drain_tty(void)
{
    unsigned char b;

    g_unget = -1;
    for (;;) {
        struct pollfd pfd;
        int pr;

        pfd.fd = g_tty;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 30);
        if (pr <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }
        if (read(g_tty, &b, 1) != 1) {
            break;
        }
    }
}

static void compact_key_fds(void)
{
    int r;
    int w = 0;

    for (r = 0; r < g_key_n; r++) {
        if (g_key_fds[r] < 0) {
            if (r == 0) {
                g_keypad = 0;
                g_keypad_grabbed = 0;
            }
            continue;
        }
        g_key_fds[w++] = g_key_fds[r];
    }
    g_key_n = w;
}

static int next_capture_event(struct Event *ev)
{
    for (;;) {
        struct pollfd pfds[33];
        int nfd;
        int i;
        int found = -1;
        int pr;

        if (g_winch) {
            g_winch = 0;
            memset(ev, 0, sizeof *ev);
            ev->kind = EV_RESIZE;
            return 0;
        }
        if (g_unget >= 0) {
            unsigned char c = (unsigned char)g_unget;

            g_unget = -1;
            return event_from_byte(ev, c);
        }
        nfd = 1 + g_key_n;
        if (nfd > (int)(sizeof pfds / sizeof pfds[0])) {
            nfd = (int)(sizeof pfds / sizeof pfds[0]);
        }
        memset(pfds, 0, sizeof pfds);
        pfds[0].fd = g_tty;
        pfds[0].events = POLLIN;
        for (i = 0; i < g_key_n && i + 1 < nfd; i++) {
            pfds[1 + i].fd = g_key_fds[i];
            pfds[1 + i].events = POLLIN;
        }
        pr = poll(pfds, (nfds_t)nfd, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            memset(ev, 0, sizeof *ev);
            ev->kind = EV_QUIT;
            return 0;
        }
        for (i = 0; i < g_key_n && i + 1 < nfd; i++) {
            short rev = pfds[1 + i].revents;
            int code;

            if (!(rev & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
                continue;
            }
            if ((rev & (POLLERR | POLLHUP | POLLNVAL)) && !(rev & POLLIN)) {
                if (i == 0 && g_keypad_grabbed) {
                    keys_ungrab(g_key_fds[i]);
                    g_keypad_grabbed = 0;
                }
                close(g_key_fds[i]);
                g_key_fds[i] = -1;
                continue;
            }
            code = keys_read_press(g_key_fds[i]);
            if (found < 0 && code >= 0) {
                found = code;
            }
        }
        compact_key_fds();
        if (found >= 0) {
            drain_tty();
            memset(ev, 0, sizeof *ev);
            ev->kind = EV_HWKEY;
            ev->code = found;
            return 0;
        }
        if (pfds[0].revents & POLLIN) {
            unsigned char c;
            ssize_t n = read(g_tty, &c, 1);

            if (n == 1) {
                return event_from_byte(ev, c);
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            memset(ev, 0, sizeof *ev);
            ev->kind = EV_QUIT;
            return 0;
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            memset(ev, 0, sizeof *ev);
            ev->kind = EV_QUIT;
            return 0;
        }
    }
}

static int next_event(struct Event *ev, int capture)
{
    unsigned char c;
    int rc;

    if (capture && g_key_n > 0) {
        return next_capture_event(ev);
    }
    memset(ev, 0, sizeof *ev);
    if (g_winch) {
        g_winch = 0;
        ev->kind = EV_RESIZE;
        return 0;
    }
    rc = read_byte(g_tty, -1, &c);
    if (rc == 0 && g_winch) {
        g_winch = 0;
        ev->kind = EV_RESIZE;
        return 0;
    }
    if (rc != 1) {
        ev->kind = EV_QUIT;
        return 0;
    }
    return event_from_byte(ev, c);
}

static int hit_ok(const struct Ui *ui, int x, int y)
{
    return ui->ok_on && ui->ok_w > 0 && y == ui->ok_r &&
           x >= ui->ok_c && x < ui->ok_c + ui->ok_w;
}

static int hit_focus(const struct Ui *ui, int x, int y)
{
    int i;

    for (i = 0; i < FE_FIELD_COUNT; i++) {
        int right = ui->box_c + ui->box_w[i] - 1;

        if (y == ui->field_r[i] && x >= ui->label_c && x <= right) {
            return i;
        }
    }
    if (y == ui->save_r && x >= ui->save_c && x < ui->save_c + SAVE_W) {
        return FE_SAVE;
    }
    if (y == ui->cancel_r && x >= ui->cancel_c && x < ui->cancel_c + CANCEL_W) {
        return FE_CANCEL;
    }
    return -1;
}

static void place_cursor(struct Editor *ed, int id, int x)
{
    size_t cap;
    char *text = field_text(&ed->edit, id, &cap);
    int interior;
    int scroll;
    int rel;
    int box_c = ed->ui.box_c;
    int box_w = ed->ui.box_w[id];

    if (!text) {
        return;
    }
    if (is_check_field(&ed->edit, id) || is_key_field(id)) {
        return;
    }
    interior = box_w - 2;
    if (interior < 1) {
        interior = 1;
    }
    if (x < box_c + 1 || x >= box_c + box_w - 1) {
        ed->cursor = (int)strlen(text);
        return;
    }
    rel = x - (box_c + 1);
    scroll = (ed->focus == id) ? field_scroll(text, ed->cursor, interior) : 0;
    ed->cursor = byte_at_column(text, scroll + rel);
}

static void try_save(struct Editor *ed)
{
    Config cfg;

    if (config_validate_edit(&ed->edit, &cfg, ed->err, sizeof ed->err) != 0) {
        snprintf(ed->dialog_title, sizeof ed->dialog_title, "Cannot save");
        ed->dialog = 1;
        return;
    }
    if (config_save(ed->path, &cfg, ed->err, sizeof ed->err) != 0) {
        if (!ed->err[0]) {
            snprintf(ed->err, sizeof ed->err, "Could not save.");
        }
        snprintf(ed->dialog_title, sizeof ed->dialog_title, "Cannot save");
        ed->dialog = 1;
        return;
    }
    ed->saved = 1;
}

static int editing_text(const struct Editor *ed)
{
    return ed->focus < FE_FIELD_COUNT && !is_check_field(&ed->edit, ed->focus) &&
           !is_key_field(ed->focus);
}

static void trim_copy(char *dest, size_t n, const char *src)
{
    size_t len;

    while (*src && isspace((unsigned char)*src)) {
        src++;
    }
    snprintf(dest, n, "%s", src ? src : "");
    len = strlen(dest);
    while (len > 0 && isspace((unsigned char)dest[len - 1])) {
        dest[--len] = '\0';
    }
}

static const char *key_field_value(const ConfigEdit *edit, int id)
{
    switch (id) {
    case FE_KEY_PAUSE:
        return edit->key_pause;
    case FE_KEY_SKIP:
        return edit->key_skip;
    case FE_KEY_UP:
        return edit->key_vol_up;
    case FE_KEY_DN:
        return edit->key_vol_dn;
    default:
        return "";
    }
}

static int key_conflict(const ConfigEdit *edit, int self, const char *name, const char **who)
{
    static const int ids[4] = {FE_KEY_PAUSE, FE_KEY_SKIP, FE_KEY_UP, FE_KEY_DN};
    int i;

    if (who) {
        *who = "";
    }
    if (!edit || !name || !name[0]) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        char got[64];

        if (ids[i] == self) {
            continue;
        }
        trim_copy(got, sizeof got, key_field_value(edit, ids[i]));
        if (got[0] && strcmp(got, name) == 0) {
            if (who) {
                *who = field_label(ids[i]);
            }
            return 1;
        }
    }
    return 0;
}

static void stop_capture(struct Editor *ed)
{
    release_keys();
    if (ed) {
        ed->capture = 0;
    }
}

static void begin_capture(struct Editor *ed, int id)
{
    int pad = 0;
    int busy = 0;

    ed->focus = id;
    ed->capture_id = id;
    ed->dialog = 0;
    release_keys();
    g_key_n = keys_open_capture(g_key_fds, (int)(sizeof g_key_fds / sizeof g_key_fds[0]), &pad);
    if (g_key_n < 0) {
        g_key_n = 0;
    }
    g_keypad = pad && g_key_n > 0;
    if (g_keypad) {
        if (keys_grab(g_key_fds[0]) == 0) {
            g_keypad_grabbed = 1;
        } else if (errno == EBUSY) {
            busy = 1;
        }
    }
    if (g_key_n == 0) {
        snprintf(ed->err, sizeof ed->err,
                 "Press a new key.\nNo keypad or keyboard was found.");
    } else if (busy) {
        snprintf(ed->err, sizeof ed->err,
                 "Press a new key.\nKeypad is in use by playback. Use another keyboard, or cancel.");
    } else {
        snprintf(ed->err, sizeof ed->err, "Press a new key.");
    }
    ed->capture = 1;
}

static int activate_key(const struct Event *ev)
{
    return ev->kind == EV_ENTER ||
           (ev->kind == EV_CHAR && ev->text[0] == ' ' && ev->text[1] == '\0');
}

static int handle_event(struct Editor *ed, const struct Event *ev)
{
    size_t cap;
    char *text;

    if (ev->kind == EV_QUIT) {
        ed->saved = 0;
        return 1;
    }
    if (ev->kind == EV_RESIZE || ev->kind == EV_NONE) {
        return 0;
    }
    if (ed->ui.too_small) {
        if (ev->kind == EV_ESC) {
            ed->saved = 0;
            return 1;
        }
        return 0;
    }
    if (ed->capture) {
        const char *name;
        const char *who = NULL;

        if (ev->kind == EV_ESC ||
            (ev->kind == EV_HWKEY && key_name(ev->code) &&
             strcmp(key_name(ev->code), "KEY_ESC") == 0) ||
            (ev->kind == EV_MOUSE && hit_ok(&ed->ui, ev->x, ev->y))) {
            stop_capture(ed);
            ed->err[0] = '\0';
            return 0;
        }
        if (ev->kind != EV_HWKEY) {
            return 0;
        }
        name = key_name(ev->code);
        if (!name || strcmp(name, "KEY_NUMLOCK") == 0) {
            return 0;
        }
        if (key_conflict(&ed->edit, ed->capture_id, name, &who)) {
            stop_capture(ed);
            snprintf(ed->err, sizeof ed->err, "%s is already used for %s.", name,
                     who ? who : "another action");
            snprintf(ed->dialog_title, sizeof ed->dialog_title, "Key in use");
            ed->dialog = 1;
            return 0;
        }
        text = field_text(&ed->edit, ed->capture_id, &cap);
        if (text) {
            snprintf(text, cap, "%s", name);
        }
        stop_capture(ed);
        ed->err[0] = '\0';
        return 0;
    }
    if (ed->dialog) {
        if (ev->kind == EV_ESC || ev->kind == EV_ENTER ||
            (ev->kind == EV_CHAR && ev->text[0] == ' ') ||
            (ev->kind == EV_MOUSE && hit_ok(&ed->ui, ev->x, ev->y))) {
            ed->dialog = 0;
        }
        return 0;
    }
    if (ev->kind == EV_ESC) {
        ed->saved = 0;
        return 1;
    }
    if (ev->kind == EV_UP || ev->kind == EV_STAB) {
        move_focus(ed, -1);
        return 0;
    }
    if (ev->kind == EV_DOWN || ev->kind == EV_TAB) {
        move_focus(ed, 1);
        return 0;
    }
    if (ev->kind == EV_MOUSE) {
        int id = hit_focus(&ed->ui, ev->x, ev->y);

        if (id < 0) {
            return 0;
        }
        if (id >= FE_FIELD_COUNT) {
            ed->focus = id;
            if (id == FE_SAVE) {
                try_save(ed);
                return ed->saved;
            }
            ed->saved = 0;
            return 1;
        }
        if (is_check_field(&ed->edit, id)) {
            ed->focus = id;
            toggle_check(ed, id);
            return 0;
        }
        if (is_key_field(id)) {
            begin_capture(ed, id);
            return 0;
        }
        place_cursor(ed, id, ev->x);
        ed->focus = id;
        return 0;
    }
    if (ed->focus == FE_SAVE && (ev->kind == EV_ENTER ||
                                 (ev->kind == EV_CHAR && ev->text[0] == ' '))) {
        try_save(ed);
        return ed->saved;
    }
    if (ed->focus == FE_CANCEL && (ev->kind == EV_ENTER ||
                                   (ev->kind == EV_CHAR && ev->text[0] == ' '))) {
        ed->saved = 0;
        return 1;
    }
    if (ed->focus >= FE_FIELD_COUNT) {
        if (ev->kind == EV_LEFT || ev->kind == EV_UP) {
            move_focus(ed, -1);
        } else if (ev->kind == EV_RIGHT || ev->kind == EV_DOWN) {
            move_focus(ed, 1);
        }
        return 0;
    }
    if (is_check_field(&ed->edit, ed->focus)) {
        if (ev->kind == EV_ENTER || (ev->kind == EV_CHAR && ev->text[0] == ' ')) {
            toggle_check(ed, ed->focus);
            return 0;
        }
        if (ev->kind == EV_LEFT) {
            move_focus(ed, -1);
        } else if (ev->kind == EV_RIGHT) {
            move_focus(ed, 1);
        }
        return 0;
    }
    if (is_key_field(ed->focus)) {
        if (activate_key(ev)) {
            begin_capture(ed, ed->focus);
            return 0;
        }
        if (ev->kind == EV_LEFT) {
            move_focus(ed, -1);
        } else if (ev->kind == EV_RIGHT) {
            move_focus(ed, 1);
        }
        return 0;
    }
    text = field_text(&ed->edit, ed->focus, &cap);
    if (!text) {
        return 0;
    }
    if (ed->cursor < 0) {
        ed->cursor = 0;
    }
    if ((size_t)ed->cursor > strlen(text)) {
        ed->cursor = (int)strlen(text);
    }
    if (ev->kind == EV_LEFT) {
        if (ed->cursor == 0) {
            move_focus(ed, -1);
        } else {
            ed->cursor = utf8_prev(text, ed->cursor);
        }
        return 0;
    }
    if (ev->kind == EV_RIGHT) {
        if (text[ed->cursor] == '\0') {
            move_focus(ed, 1);
        } else {
            ed->cursor += utf8_span((const unsigned char *)text + ed->cursor);
        }
        return 0;
    }
    if (ev->kind == EV_HOME) {
        ed->cursor = 0;
        return 0;
    }
    if (ev->kind == EV_END) {
        ed->cursor = (int)strlen(text);
        return 0;
    }
    if (ev->kind == EV_BACKSPACE) {
        text_backspace(text, &ed->cursor);
        return 0;
    }
    if (ev->kind == EV_DELETE) {
        text_delete(text, &ed->cursor);
        return 0;
    }
    if (ev->kind == EV_ENTER) {
        move_focus(ed, 1);
        return 0;
    }
    if (ev->kind == EV_CHAR && editing_text(ed)) {
        text_insert(text, cap, &ed->cursor, ev->text);
    }
    return 0;
}

int config_edit(const char *path)
{
    struct Editor ed;
    size_t cap;
    char *text;

    if (!path || !path[0]) {
        log_error("autodj", "config path is missing");
        return -1;
    }
    memset(&ed, 0, sizeof ed);
    ed.path = path;
    if (config_load_edit(path, &ed.edit) != 0) {
        return -1;
    }
    text = field_text(&ed.edit, FE_FOLDER, &cap);
    ed.cursor = text ? (int)strlen(text) : 0;
    g_unget = -1;
    g_winch = 0;
    if (enter_tty() != 0) {
        return -1;
    }
    for (;;) {
        struct Event ev;

        if (render(&ed) != 0) {
            leave_tty();
            log_error("autodj", "cannot draw the config screen");
            return -1;
        }
        if (next_event(&ev, ed.capture) != 0) {
            break;
        }
        if (handle_event(&ed, &ev)) {
            break;
        }
    }
    leave_tty();
    if (ed.saved) {
        log_info("autodj", "saved %s", path);
    }
    return 0;
}

int cfgedit_self_test(void)
{
    ConfigEdit edit;
    const char *who = "x";

    memset(&edit, 0, sizeof edit);
    snprintf(edit.key_pause, sizeof edit.key_pause, "KEY_KP0");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KPDOT");
    snprintf(edit.key_vol_up, sizeof edit.key_vol_up, " KEY_KPPLUS ");
    if (key_conflict(&edit, FE_KEY_SKIP, "KEY_KP0", &who) != 1 ||
        !who || strcmp(who, "Pause/resume") != 0) {
        log_error("cfgedit", "duplicate pause key was not reported");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_PAUSE, "KEY_KP0", &who) != 0 || !who || who[0] != '\0') {
        log_error("cfgedit", "the same field was treated as a duplicate");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_DN, "KEY_KPPLUS", &who) != 1 ||
        !who || strcmp(who, "Volume up") != 0) {
        log_error("cfgedit", "trimmed duplicate was not reported");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_DN, "KEY_KPASTERISK", &who) != 0) {
        log_error("cfgedit", "a free key was rejected");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_DN, "", &who) != 0) {
        log_error("cfgedit", "an empty key was treated as a duplicate");
        return -1;
    }
    return 0;
}
