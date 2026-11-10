#include "cfgedit.h"

#include "audio.h"
#include "config.h"
#include "decode.h"
#include "keys.h"
#include "log.h"
#include "player.h"
#include "playlist.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

/*
 * Full-screen editor for master.cfg. The tabs are Main, Buttons, Reverb,
 * Chipmunk, Autopan, Phaser, Chorus, and Flanger. They stay on one row when
 * the window is wide enough. Chorus and Flanger wrap onto a second row when
 * it is not.
 * Tab and the arrow keys move between the fields on the current tab. Up from
 * the first field reaches the tabs, and left and right there switch tabs.
 * Left and right move the cursor in a text field, and step to the next field
 * at either end. A key binding is captured from the keypad, or from another
 * keyboard if the press is not on the keypad. Esc or Cancel leaves that
 * binding unchanged. Delete on the key prompt removes it. Save validates
 * before it writes.
 */

enum {
    TAB_MAIN = 0,
    TAB_BUTTONS,
    TAB_REVERB,
    TAB_PITCH,
    TAB_AUTOPAN,
    TAB_PHASER,
    TAB_CHORUS,
    TAB_FLANGER,
    TAB_COUNT
};

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
    FE_DIAL_DEFAULT,
    FE_MAIN_END,

    FE_KEY_PAUSE = FE_MAIN_END,
    FE_KEY_SKIP,
    FE_KEY_PREV,
    FE_KEY_REW05,
    FE_KEY_REW10,
    FE_KEY_REW15,
    FE_KEY_DIAL_FWD,
    FE_KEY_DIAL_BACK,
    FE_KEY_DIAL_MODE,
    FE_KEY_REVERB,
    FE_KEY_PITCH,
    FE_KEY_AUTOPAN,
    FE_KEY_PHASER,
    FE_KEY_CHORUS,
    FE_KEY_FLANGER,
    FE_KEY_EFFECTS_OFF,
    FE_BUTTON_END,

    FE_RV_DECAY = FE_BUTTON_END,
    FE_RV_BW,
    FE_RV_DAMP,
    FE_RV_SEND,
    FE_RV_END,

    FE_PS_SEMI = FE_RV_END,
    FE_PS_FORMANT,
    FE_PS_PITCH,
    FE_PS_RANGE,
    FE_PS_END,

    FE_AP_PERIOD = FE_PS_END,
    FE_AP_SMOOTH,
    FE_AP_DISTANCE,
    FE_AP_END,

    FE_PH_PERIOD = FE_AP_END,
    FE_PH_LOW,
    FE_PH_HIGH,
    FE_PH_STAGES,
    FE_PH_FEEDBACK,
    FE_PH_MIX,
    FE_PH_BOOST,
    FE_PH_END,

    FE_CH_PERIOD = FE_PH_END,
    FE_CH_DELAY,
    FE_CH_DEPTH,
    FE_CH_VOICES,
    FE_CH_MIX,
    FE_CH_END,

    FE_FL_PERIOD = FE_CH_END,
    FE_FL_DELAY,
    FE_FL_DEPTH,
    FE_FL_FEEDBACK,
    FE_FL_MIX,
    FE_FIELD_COUNT,

    FE_TABS = FE_FIELD_COUNT,
    FE_SAVE,
    FE_CANCEL,
    FE_COUNT
};

enum {
    LABEL_W = 16,
    NUM_W = 10,
    /* Tallest tab is Buttons, with 16 fields, under one row of tabs.
     * A wrapped tab row adds one line, checked in layout_ui. */
    TAB_FIELDS = FE_BUTTON_END - FE_KEY_PAUSE,
    ROW_TABS = 1,
    ROW_NOTE = 2,
    ROW_FIELD0 = 3,
    ROW_BUTTONS = ROW_FIELD0 + TAB_FIELDS,
    FRAME_H = ROW_BUTTONS + 2,
    /* The hint line below the frame is left out when it does not fit. */
    MIN_ROWS = FRAME_H,
    MIN_COLS = 72,
    SAVE_W = 8,
    CANCEL_W = 10
};

/* Which button of the key prompt is highlighted. Cancel is the default. */
enum {
    CAP_CANCEL = 0,
    CAP_DELETE = 1
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
    int tab_y[TAB_COUNT];
    int tab_c[TAB_COUNT];
    int tab_w[TAB_COUNT];
    int note_r;
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
    int del_on;
    int del_r;
    int del_c;
    int del_w;
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
    int capture_btn;
    int tab;
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
static int g_key_grabbed[32];

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
        "Dial starts in",
        "Pause/resume",
        "Skip",
        "Previous",
        "Rewind 0.5s",
        "Rewind 1s",
        "Rewind 1.5s",
        "Dial forward",
        "Dial back",
        "Dial mode",
        "Reverb",
        "Chipmunk",
        "Autopan",
        "Phaser",
        "Chorus",
        "Flanger",
        "All effects off",
        "Decay",
        "BW",
        "Damping",
        "Send",
        "Semitones",
        "Formant",
        "Pitch",
        "Range",
        "Period",
        "Smoothing",
        "Distance %",
        "Period",
        "Low Hz",
        "High Hz",
        "Stages",
        "Feedback",
        "Mix %",
        "Boost %",
        "Period",
        "Delay ms",
        "Depth ms",
        "Voices",
        "Mix %",
        "Period",
        "Delay ms",
        "Depth ms",
        "Feedback",
        "Mix %"
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
    case FE_DIAL_DEFAULT:
        *cap = sizeof edit->dial_default;
        return edit->dial_default;
    case FE_KEY_PAUSE:
        *cap = sizeof edit->key_pause;
        return edit->key_pause;
    case FE_KEY_SKIP:
        *cap = sizeof edit->key_skip;
        return edit->key_skip;
    case FE_KEY_PREV:
        *cap = sizeof edit->key_prev;
        return edit->key_prev;
    case FE_KEY_REW05:
        *cap = sizeof edit->key_rew05;
        return edit->key_rew05;
    case FE_KEY_REW10:
        *cap = sizeof edit->key_rew10;
        return edit->key_rew10;
    case FE_KEY_REW15:
        *cap = sizeof edit->key_rew15;
        return edit->key_rew15;
    case FE_KEY_DIAL_FWD:
        *cap = sizeof edit->key_dial_fwd;
        return edit->key_dial_fwd;
    case FE_KEY_DIAL_BACK:
        *cap = sizeof edit->key_dial_back;
        return edit->key_dial_back;
    case FE_KEY_DIAL_MODE:
        *cap = sizeof edit->key_dial_mode;
        return edit->key_dial_mode;
    case FE_KEY_REVERB:
        *cap = sizeof edit->key_reverb;
        return edit->key_reverb;
    case FE_KEY_PITCH:
        *cap = sizeof edit->key_pitch;
        return edit->key_pitch;
    case FE_KEY_AUTOPAN:
        *cap = sizeof edit->key_autopan;
        return edit->key_autopan;
    case FE_KEY_PHASER:
        *cap = sizeof edit->key_phaser;
        return edit->key_phaser;
    case FE_KEY_CHORUS:
        *cap = sizeof edit->key_chorus;
        return edit->key_chorus;
    case FE_KEY_FLANGER:
        *cap = sizeof edit->key_flanger;
        return edit->key_flanger;
    case FE_KEY_EFFECTS_OFF:
        *cap = sizeof edit->key_effects_off;
        return edit->key_effects_off;
    case FE_RV_DECAY:
        *cap = sizeof edit->rv_decay;
        return edit->rv_decay;
    case FE_RV_BW:
        *cap = sizeof edit->rv_bw;
        return edit->rv_bw;
    case FE_RV_DAMP:
        *cap = sizeof edit->rv_damp;
        return edit->rv_damp;
    case FE_RV_SEND:
        *cap = sizeof edit->rv_send;
        return edit->rv_send;
    case FE_PS_SEMI:
        *cap = sizeof edit->ps_semi;
        return edit->ps_semi;
    case FE_PS_FORMANT:
        *cap = sizeof edit->ps_formant;
        return edit->ps_formant;
    case FE_PS_PITCH:
        *cap = sizeof edit->ps_pitch;
        return edit->ps_pitch;
    case FE_PS_RANGE:
        *cap = sizeof edit->ps_range;
        return edit->ps_range;
    case FE_AP_PERIOD:
        *cap = sizeof edit->ap_period;
        return edit->ap_period;
    case FE_AP_SMOOTH:
        *cap = sizeof edit->ap_smooth;
        return edit->ap_smooth;
    case FE_AP_DISTANCE:
        *cap = sizeof edit->ap_distance;
        return edit->ap_distance;
    case FE_PH_PERIOD:
        *cap = sizeof edit->ph_period;
        return edit->ph_period;
    case FE_PH_LOW:
        *cap = sizeof edit->ph_low;
        return edit->ph_low;
    case FE_PH_HIGH:
        *cap = sizeof edit->ph_high;
        return edit->ph_high;
    case FE_PH_STAGES:
        *cap = sizeof edit->ph_stages;
        return edit->ph_stages;
    case FE_PH_FEEDBACK:
        *cap = sizeof edit->ph_feedback;
        return edit->ph_feedback;
    case FE_PH_MIX:
        *cap = sizeof edit->ph_mix;
        return edit->ph_mix;
    case FE_PH_BOOST:
        *cap = sizeof edit->ph_boost;
        return edit->ph_boost;
    case FE_CH_PERIOD:
        *cap = sizeof edit->ch_period;
        return edit->ch_period;
    case FE_CH_DELAY:
        *cap = sizeof edit->ch_delay;
        return edit->ch_delay;
    case FE_CH_DEPTH:
        *cap = sizeof edit->ch_depth;
        return edit->ch_depth;
    case FE_CH_VOICES:
        *cap = sizeof edit->ch_voices;
        return edit->ch_voices;
    case FE_CH_MIX:
        *cap = sizeof edit->ch_mix;
        return edit->ch_mix;
    case FE_FL_PERIOD:
        *cap = sizeof edit->fl_period;
        return edit->fl_period;
    case FE_FL_DELAY:
        *cap = sizeof edit->fl_delay;
        return edit->fl_delay;
    case FE_FL_DEPTH:
        *cap = sizeof edit->fl_depth;
        return edit->fl_depth;
    case FE_FL_FEEDBACK:
        *cap = sizeof edit->fl_feedback;
        return edit->fl_feedback;
    case FE_FL_MIX:
        *cap = sizeof edit->fl_mix;
        return edit->fl_mix;
    default:
        *cap = 0;
        return NULL;
    }
}

static int is_number_field(int id)
{
    return id == FE_OVERLAP || id == FE_PAUSE || id == FE_RESUME ||
           id == FE_VOLUME || id == FE_LUFS || id == FE_RV_DECAY ||
           id == FE_RV_BW || id == FE_RV_DAMP || id == FE_RV_SEND ||
           id == FE_PS_PITCH || id == FE_PS_RANGE || id == FE_AP_PERIOD ||
           id == FE_AP_SMOOTH || id == FE_AP_DISTANCE ||
           (id >= FE_PH_PERIOD && id <= FE_PH_BOOST) ||
           (id >= FE_CH_PERIOD && id <= FE_CH_MIX) ||
           (id >= FE_FL_PERIOD && id <= FE_FL_MIX);
}

static int is_key_field(int id)
{
    return id >= FE_KEY_PAUSE && id < FE_BUTTON_END;
}

static int is_check_field(const ConfigEdit *edit, int id)
{
    size_t cap;
    int on = 0;
    char *text;

    if (id != FE_SHUFFLE && id != FE_TRIM && id != FE_LOUDNESS &&
        id != FE_PS_SEMI && id != FE_PS_FORMANT) {
        return 0;
    }
    text = field_text((ConfigEdit *)edit, id, &cap);
    return text && config_parse_bool(text, &on) == 0;
}

/* A field that Space switches between two words. A value that is neither
 * stays a text field so it can be corrected. */
static int is_choice_field(const ConfigEdit *edit, int id)
{
    size_t cap;
    int on = 0;
    char *text;

    if (id != FE_DIAL_DEFAULT) {
        return 0;
    }
    text = field_text((ConfigEdit *)edit, id, &cap);
    return text && config_parse_dial(text, &on) == 0;
}

static int is_toggle_field(const ConfigEdit *edit, int id)
{
    return is_check_field(edit, id) || is_choice_field(edit, id);
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

    if (text && id == FE_DIAL_DEFAULT && config_parse_dial(text, &on) == 0) {
        snprintf(text, cap, "%s", on ? "jog" : "volume");
        return;
    }
    if (!text || config_parse_bool(text, &on) != 0) {
        return;
    }
    snprintf(text, cap, "%s", on ? "false" : "true");
}

static int tab_first(int tab)
{
    static const int first[] = {
        FE_FOLDER, FE_KEY_PAUSE, FE_RV_DECAY, FE_PS_SEMI, FE_AP_PERIOD,
        FE_PH_PERIOD, FE_CH_PERIOD, FE_FL_PERIOD
    };

    if (tab < 0 || tab >= TAB_COUNT) {
        return FE_FOLDER;
    }
    return first[tab];
}

static int tab_last(int tab)
{
    static const int last[] = {
        FE_MAIN_END - 1, FE_BUTTON_END - 1, FE_RV_END - 1, FE_PS_END - 1,
        FE_AP_END - 1, FE_PH_END - 1, FE_CH_END - 1, FE_FIELD_COUNT - 1
    };

    if (tab < 0 || tab >= TAB_COUNT) {
        return FE_DIAL_DEFAULT;
    }
    return last[tab];
}

static void focus_field(struct Editor *ed, int id)
{
    size_t cap;
    char *text;

    ed->focus = id;
    text = field_text(&ed->edit, id, &cap);
    ed->cursor = text ? (int)strlen(text) : 0;
}

static void change_tab(struct Editor *ed, int tab)
{
    if (tab < 0) {
        tab = TAB_COUNT - 1;
    }
    if (tab >= TAB_COUNT) {
        tab = 0;
    }
    ed->tab = tab;
}

static void move_focus(struct Editor *ed, int delta)
{
    int id = ed->focus;

    if (id == FE_TABS) {
        if (delta > 0) {
            focus_field(ed, tab_first(ed->tab));
        } else {
            ed->focus = FE_CANCEL;
        }
        return;
    }
    if (id == FE_SAVE) {
        if (delta > 0) {
            ed->focus = FE_CANCEL;
        } else {
            focus_field(ed, tab_last(ed->tab));
        }
        return;
    }
    if (id == FE_CANCEL) {
        if (delta > 0) {
            ed->focus = FE_TABS;
        } else {
            ed->focus = FE_SAVE;
        }
        return;
    }
    if (id < tab_first(ed->tab) || id > tab_last(ed->tab)) {
        focus_field(ed, tab_first(ed->tab));
        return;
    }
    if (delta > 0) {
        if (id >= tab_last(ed->tab)) {
            ed->focus = FE_SAVE;
        } else {
            focus_field(ed, id + 1);
        }
    } else if (id <= tab_first(ed->tab)) {
        ed->focus = FE_TABS;
    } else {
        focus_field(ed, id - 1);
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

static int field_on_tab(int id, int tab)
{
    return id >= tab_first(tab) && id <= tab_last(tab);
}

static const char *tab_title(int tab);

/* "[ Name ]" plus the space before the next tab. */
static int tab_width(int tab)
{
    return (int)strlen(tab_title(tab)) + 4;
}

/* How many rows the tabs need inside a frame of this width. */
static int tab_row_count(int frame_w)
{
    int limit = frame_w - 3;
    int c = 0;
    int rows = 1;
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        int w = tab_width(i);

        if (c > 0 && c + w > limit) {
            rows++;
            c = 0;
        }
        c += w + 1;
    }
    return rows;
}

static void layout_ui(struct Ui *ui, const ConfigEdit *edit, int rows, int cols, int tab)
{
    int avail;
    int i;
    int row;
    int total;
    int start;
    int extra;

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
    extra = tab_row_count(ui->frame_w) - 1;
    ui->frame_h = FRAME_H + extra;
    if (rows < ui->frame_h) {
        ui->too_small = 1;
        return;
    }
    ui->frame_r = (rows - ui->frame_h - 1) / 2 + 1;
    if (ui->frame_r < 1) {
        ui->frame_r = 1;
    }
    ui->frame_c = (cols - ui->frame_w) / 2 + 1;
    ui->label_c = ui->frame_c + 2;
    ui->box_c = ui->label_c + LABEL_W + 1;
    ui->note_r = ui->frame_r + ROW_NOTE + extra;
    avail = ui->frame_w - 4 - LABEL_W - 1;
    if (avail < NUM_W) {
        ui->too_small = 1;
        return;
    }
    row = 0;
    for (i = 0; i < FE_FIELD_COUNT; i++) {
        if (!field_on_tab(i, tab)) {
            ui->field_r[i] = -1;
            ui->box_w[i] = 0;
            continue;
        }
        ui->field_r[i] = ui->frame_r + ROW_FIELD0 + extra + row;
        row++;
        if (is_check_field(edit, i)) {
            ui->box_w[i] = 3;
        } else if (is_number_field(i) || i == FE_DIAL_DEFAULT) {
            ui->box_w[i] = NUM_W;
        } else {
            ui->box_w[i] = avail;
        }
    }
    ui->save_r = ui->frame_r + ROW_BUTTONS + extra;
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

/* extra is an optional button drawn to the left of button. focus is 1 when
 * extra is highlighted, and 0 when button is highlighted. */
static void draw_dialog(FILE *fp, struct Ui *ui, const char *title, const char *msg,
                        const char *button, const char *extra, int focus)
{
    char lines[16][96];
    int max_lines = ui->rows - 8;
    int text_w = ui->cols - 8;
    int nlines;
    int box_w;
    int box_h;
    int box_r;
    int box_c;
    int buttons;
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
    if (!extra || !extra[0]) {
        extra = NULL;
        focus = 0;
    }
    if (!msg || !msg[0]) {
        msg = "Cannot save.";
    }
    buttons = (int)strlen(button);
    if (extra) {
        buttons += 2 + (int)strlen(extra);
    }
    if (text_w < buttons) {
        text_w = buttons;
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
    ui->del_on = 0;
    ui->ok_r = box_r + nlines + 4;
    if (extra) {
        int gap = 2;
        int ew = (int)strlen(extra);
        int bw = (int)strlen(button);
        int total = ew + gap + bw;
        int left = box_c + (box_w - total) / 2;

        if (left < box_c + 1) {
            left = box_c + 1;
        }
        ui->del_on = 1;
        ui->del_r = ui->ok_r;
        ui->del_c = left;
        ui->del_w = ew;
        ui->ok_on = 1;
        ui->ok_w = bw;
        ui->ok_c = left + ew + gap;
        if (ui->ok_c + ui->ok_w > box_c + box_w - 1) {
            ui->ok_c = box_c + box_w - 1 - ui->ok_w;
        }
        draw_button(fp, ui->del_r, ui->del_c, extra, focus == 1);
        draw_button(fp, ui->ok_r, ui->ok_c, button, focus == 0);
    } else {
        ui->ok_on = 1;
        ui->ok_w = (int)strlen(button);
        ui->ok_c = box_c + (box_w - ui->ok_w) / 2;
        if (ui->ok_c < box_c + 1) {
            ui->ok_c = box_c + 1;
        }
        draw_button(fp, ui->ok_r, ui->ok_c, button, 1);
    }
}

static const char *tab_title(int tab)
{
    static const char *names[] = {
        "Main", "Buttons", "Reverb", "Chipmunk",
        "Autopan", "Phaser", "Chorus", "Flanger"
    };

    if (tab < 0 || tab >= TAB_COUNT) {
        return "";
    }
    return names[tab];
}

static const char *tab_note(int tab)
{
    static const char *notes[] = {
        "Folder, loudness, and the dial.",
        "Buttons. Volume is on the dial.",
        "Room. Send is how much reverb is added.",
        "Pitch 1 is an octave up. 0 is unchanged.",
        "Left to right and back. Period counts beats.",
        "Notches sweep from Low to High. Period is in measures.",
        "Several slightly late copies, drifting in pitch.",
        "A very short delay. The sweep makes a jet-like whoosh."
    };

    if (tab < 0 || tab >= TAB_COUNT) {
        return "";
    }
    return notes[tab];
}

static void draw_tabs(struct Editor *ed, FILE *fp)
{
    struct Ui *ui = &ed->ui;
    int limit = ui->frame_c + ui->frame_w - 2;
    int c = ui->label_c;
    int row = ui->frame_r + ROW_TABS;
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        char label[24];
        int w;

        snprintf(label, sizeof label, "[ %s ]", tab_title(i));
        w = (int)strlen(label);
        if (c > ui->label_c && c + w - 1 > limit) {
            c = ui->label_c;
            row++;
        }
        ui->tab_y[i] = row;
        ui->tab_c[i] = c;
        ui->tab_w[i] = w;
        draw_button(fp, row, c, label, ed->tab == i);
        c += w + 1;
    }
}

static void draw_screen(struct Editor *ed, FILE *fp)
{
    struct Ui *ui = &ed->ui;
    int i;
    const char *hint = "Tab moves   Up for tabs, left and right switch   Esc cancels";
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
    {
        char shown[AUTODJ_PATH];
        char title[AUTODJ_PATH + 32];
        int width = ui->frame_w - 4 - (int)strlen(" autodj config:  ");

        shorten_path(ed->path, width, shown, sizeof shown);
        snprintf(title, sizeof title, " autodj config: %s ", shown);
        draw_hbar(fp, ui->frame_r, ui->frame_c, ui->frame_w, title);
    }
    draw_hbar(fp, ui->frame_r + ui->frame_h - 1, ui->frame_c, ui->frame_w, NULL);
    for (i = 1; i < ui->frame_h - 1; i++) {
        cup(fp, ui->frame_r + i, ui->frame_c);
        fputc('|', fp);
        cup(fp, ui->frame_r + i, ui->frame_c + ui->frame_w - 1);
        fputc('|', fp);
    }
    draw_tabs(ed, fp);
    cup(fp, ui->note_r, ui->label_c);
    fputs(tab_note(ed->tab), fp);
    for (i = 0; i < FE_FIELD_COUNT; i++) {
        size_t cap;
        char *text = field_text(&ed->edit, i, &cap);
        int focused = ed->focus == i;
        int on = 0;

        if (ui->field_r[i] < 0) {
            continue;
        }
        cup(fp, ui->field_r[i], ui->label_c);
        if (focused) {
            fputs("\033[1m", fp);
        }
        fprintf(fp, "%-*s", LABEL_W, field_label(i));
        fputs("\033[0m", fp);
        if (is_check_field(&ed->edit, i)) {
            config_parse_bool(text, &on);
            draw_check(fp, ui->field_r[i], ui->box_c, on, focused);
        } else if (is_key_field(i) || is_choice_field(&ed->edit, i)) {
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
                    ed->err[0] ? ed->err : "Press a new key.", "[ Cancel ]",
                    "[ Delete ]", ed->capture_btn == CAP_DELETE);
    } else if (ed->dialog) {
        draw_dialog(fp, ui, ed->dialog_title, ed->err, "[ OK ]", NULL, 0);
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
    layout_ui(&ed->ui, &ed->edit, rows, cols, ed->tab);
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
    int i;

    for (i = 0; i < g_key_n; i++) {
        if (g_key_grabbed[i]) {
            keys_ungrab(g_key_fds[i]);
        }
        g_key_grabbed[i] = 0;
    }
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
            g_key_grabbed[r] = 0;
            continue;
        }
        g_key_grabbed[w] = g_key_grabbed[r];
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
                if (g_key_grabbed[i]) {
                    keys_ungrab(g_key_fds[i]);
                    g_key_grabbed[i] = 0;
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

static int hit_del(const struct Ui *ui, int x, int y)
{
    return ui->del_on && ui->del_w > 0 && y == ui->del_r &&
           x >= ui->del_c && x < ui->del_c + ui->del_w;
}

static int hit_tab(const struct Ui *ui, int x, int y)
{
    int i;

    for (i = 0; i < TAB_COUNT; i++) {
        if (y == ui->tab_y[i] && x >= ui->tab_c[i] && x < ui->tab_c[i] + ui->tab_w[i]) {
            return i;
        }
    }
    return -1;
}

static int hit_focus(const struct Ui *ui, int x, int y)
{
    int i;

    for (i = 0; i < FE_FIELD_COUNT; i++) {
        int right;

        if (ui->field_r[i] < 0) {
            continue;
        }
        right = ui->box_c + ui->box_w[i] - 1;
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
    if (is_toggle_field(&ed->edit, id) || is_key_field(id)) {
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
    return ed->focus < FE_FIELD_COUNT && !is_toggle_field(&ed->edit, ed->focus) &&
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
    case FE_KEY_PREV:
        return edit->key_prev;
    case FE_KEY_REW05:
        return edit->key_rew05;
    case FE_KEY_REW10:
        return edit->key_rew10;
    case FE_KEY_REW15:
        return edit->key_rew15;
    case FE_KEY_DIAL_FWD:
        return edit->key_dial_fwd;
    case FE_KEY_DIAL_BACK:
        return edit->key_dial_back;
    case FE_KEY_DIAL_MODE:
        return edit->key_dial_mode;
    case FE_KEY_REVERB:
        return edit->key_reverb;
    case FE_KEY_PITCH:
        return edit->key_pitch;
    case FE_KEY_AUTOPAN:
        return edit->key_autopan;
    case FE_KEY_PHASER:
        return edit->key_phaser;
    case FE_KEY_CHORUS:
        return edit->key_chorus;
    case FE_KEY_FLANGER:
        return edit->key_flanger;
    case FE_KEY_EFFECTS_OFF:
        return edit->key_effects_off;
    default:
        return "";
    }
}

static int key_conflict(const ConfigEdit *edit, int self, const char *name, const char **who)
{
    static const int ids[] = {
        FE_KEY_PAUSE, FE_KEY_SKIP, FE_KEY_PREV, FE_KEY_REW05,
        FE_KEY_REW10, FE_KEY_REW15, FE_KEY_DIAL_FWD, FE_KEY_DIAL_BACK,
        FE_KEY_DIAL_MODE, FE_KEY_REVERB, FE_KEY_PITCH, FE_KEY_AUTOPAN,
        FE_KEY_PHASER, FE_KEY_CHORUS, FE_KEY_FLANGER, FE_KEY_EFFECTS_OFF
    };
    int i;

    if (who) {
        *who = "";
    }
    if (!edit || !name || !name[0]) {
        return 0;
    }
    for (i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++) {
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

static void clear_key_binding(struct Editor *ed)
{
    size_t cap;
    char *text = field_text(&ed->edit, ed->capture_id, &cap);

    if (text && cap > 0) {
        text[0] = '\0';
    }
    stop_capture(ed);
    ed->err[0] = '\0';
}

static void set_capture_prompt(struct Editor *ed, int busy)
{
    char shown[64];
    const char *cur = key_field_value(&ed->edit, ed->capture_id);
    const char *extra = NULL;

    trim_copy(shown, sizeof shown, cur ? cur : "");
    if (g_key_n == 0) {
        extra = "No keypad or keyboard was found.";
    } else if (busy) {
        extra = "Keypad is in use by playback. Use another keyboard, or cancel.";
    }
    if (shown[0] && extra) {
        snprintf(ed->err, sizeof ed->err,
                 "Press a new key, or Delete to remove %s.\n%s", shown, extra);
    } else if (shown[0]) {
        snprintf(ed->err, sizeof ed->err,
                 "Press a new key, or Delete to remove %s.", shown);
    } else if (extra) {
        snprintf(ed->err, sizeof ed->err, "Press a new key.\n%s", extra);
    } else {
        snprintf(ed->err, sizeof ed->err, "Press a new key.");
    }
}

static void begin_capture(struct Editor *ed, int id)
{
    int pad = 0;
    int busy = 0;
    int i;

    ed->focus = id;
    ed->capture_id = id;
    ed->capture_btn = CAP_CANCEL;
    ed->dialog = 0;
    release_keys();
    g_key_n = keys_open_capture(g_key_fds, (int)(sizeof g_key_fds / sizeof g_key_fds[0]), &pad);
    if (g_key_n < 0) {
        g_key_n = 0;
    }
    for (i = 0; i < pad && i < g_key_n; i++) {
        if (keys_grab(g_key_fds[i]) == 0) {
            g_key_grabbed[i] = 1;
        } else if (errno == EBUSY) {
            busy = 1;
        }
    }
    set_capture_prompt(ed, busy);
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
        int delete_it = 0;
        int cancel_it = 0;

        if (ev->kind == EV_LEFT || ev->kind == EV_RIGHT ||
            ev->kind == EV_TAB || ev->kind == EV_STAB) {
            ed->capture_btn = ed->capture_btn == CAP_DELETE ? CAP_CANCEL : CAP_DELETE;
            return 0;
        }
        if (ev->kind == EV_MOUSE && hit_del(&ed->ui, ev->x, ev->y)) {
            delete_it = 1;
        } else if (ev->kind == EV_MOUSE && hit_ok(&ed->ui, ev->x, ev->y)) {
            cancel_it = 1;
        } else if (activate_key(ev)) {
            if (ed->capture_btn == CAP_DELETE) {
                delete_it = 1;
            } else {
                cancel_it = 1;
            }
        } else if (ev->kind == EV_ESC ||
                   (ev->kind == EV_HWKEY && key_name(ev->code) &&
                    strcmp(key_name(ev->code), "KEY_ESC") == 0)) {
            cancel_it = 1;
        }
        if (delete_it) {
            clear_key_binding(ed);
            return 0;
        }
        if (cancel_it) {
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
        int tab = hit_tab(&ed->ui, ev->x, ev->y);
        int id;

        if (tab >= 0) {
            change_tab(ed, tab);
            focus_field(ed, tab_first(tab));
            return 0;
        }
        id = hit_focus(&ed->ui, ev->x, ev->y);
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
        if (is_toggle_field(&ed->edit, id)) {
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
    if (ed->focus == FE_TABS) {
        if (ev->kind == EV_LEFT) {
            change_tab(ed, ed->tab - 1);
        } else if (ev->kind == EV_RIGHT) {
            change_tab(ed, ed->tab + 1);
        }
        return 0;
    }
    if (ed->focus >= FE_FIELD_COUNT) {
        if (ev->kind == EV_LEFT || ev->kind == EV_UP) {
            move_focus(ed, -1);
        } else if (ev->kind == EV_RIGHT || ev->kind == EV_DOWN) {
            move_focus(ed, 1);
        }
        return 0;
    }
    if (is_toggle_field(&ed->edit, ed->focus)) {
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
    ed.tab = TAB_MAIN;
    ed.focus = FE_TABS;
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

/*
 * Dial calibration view (autodj --calibrate). Shows the [Jog] settings,
 * counts the clicks of the dial on the remote, measures one turn and a
 * fast spin, and plays a random song so a change can be heard at once.
 */

enum {
    DF_TURN = 0,
    DF_CLICKS,
    DF_SPEED,
    DF_SPINUP,
    DF_GRAB,
    DF_SMOOTH,
    DF_FIELD_COUNT,
    DF_CALIBRATE = DF_FIELD_COUNT,
    DF_TEST,
    DF_SAVE,
    DF_CLOSE,
    DF_COUNT
};

enum {
    DIAL_STEP_NONE = 0,
    DIAL_STEP_TURN,
    DIAL_STEP_SPIN_WAIT,
    DIAL_STEP_SPIN
};

enum {
    DIAL_LABEL_W = 18,
    DIAL_BOX_W = 10,
    DIAL_MIN_ROWS = 20,
    DIAL_MIN_COLS = 60,
    DIAL_SPIN_US = 3000000,
    DIAL_SPIN_WINDOW = 5,
    DIAL_MAX_TIMES = 2048
};

static const char *g_dial_labels[DF_FIELD_COUNT] = {
    "Seconds per turn", "Clicks per turn", "Max speed",
    "Spin-up time", "Grab time", "Smoothing"
};

static const char *g_dial_help[DF_FIELD_COUNT] = {
    "Song time moved by one full turn. More moves further per click.",
    "Dial clicks in one full turn. Calibrate counts them.",
    "Fastest scratch, times normal speed. Lower tames hard spins.",
    "Seconds to get back to normal speed after you let go.",
    "Seconds for the song to stop under your hand. 0 is instant.",
    "Seconds for the speed to follow your hand. More is smoother."
};

static const char *g_dial_buttons[DF_COUNT - DF_FIELD_COUNT] = {
    "[ Calibrate ]", "[ Test ]", "[ Save ]", "[ Close ]"
};

struct Dial {
    const char *path;
    Config cfg;
    char text[DF_FIELD_COUNT][16];
    char saved_text[DF_FIELD_COUNT][16];
    int focus;
    int cursor;
    int step;
    int testing;
    int confirm_close;
    int kfd[8];
    int kfd_n;
    long clicks;
    long net;
    long step_net;
    long long last_us;
    long long gap_us;
    long long spin_start_us;
    long long spin_times[DIAL_MAX_TIMES];
    int spin_n;
    double fastest;
    Playlist *pl;
    int decoding;
    char status[256];
    /* screen layout */
    int too_small;
    int field_r[DF_FIELD_COUNT];
    int box_c;
    int button_r;
    int button_c[DF_COUNT - DF_FIELD_COUNT];
};

static struct Dial *g_dial;

static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void dial_hook(int direction, long long when_us)
{
    struct Dial *d = g_dial;

    if (!d) {
        return;
    }
    d->clicks++;
    d->net += direction;
    if (d->last_us) {
        d->gap_us = when_us - d->last_us;
    }
    d->last_us = when_us;
    if (d->step == DIAL_STEP_TURN) {
        d->step_net += direction;
    } else if (d->step == DIAL_STEP_SPIN_WAIT) {
        d->step = DIAL_STEP_SPIN;
        d->spin_start_us = when_us;
        d->spin_n = 0;
    }
    if (d->step == DIAL_STEP_SPIN && d->spin_n < DIAL_MAX_TIMES) {
        d->spin_times[d->spin_n++] = when_us;
    }
}

/* Parse field id. Returns 0 and stores the value, or -1 if out of range. */
static int dial_parse(int id, const char *text, double *out)
{
    static const double lo[DF_FIELD_COUNT] = {
        JOG_TURN_MIN, JOG_CLICKS_MIN, JOG_SPEED_MIN, 0, 0, 0
    };
    static const double hi[DF_FIELD_COUNT] = {
        JOG_TURN_MAX, JOG_CLICKS_MAX, JOG_SPEED_MAX, JOG_SPINUP_MAX, JOG_GRAB_MAX, JOG_SMOOTH_MAX
    };
    char buf[32];
    char *end = NULL;
    double v;

    trim_copy(buf, sizeof buf, text);
    if (!buf[0]) {
        return -1;
    }
    errno = 0;
    v = strtod(buf, &end);
    if (errno != 0 || !end || *end != '\0' || v != v || v < lo[id] || v > hi[id]) {
        return -1;
    }
    if (id == DF_CLICKS && v != (double)(int)v) {
        return -1;
    }
    *out = v;
    return 0;
}

static void dial_range(int id, char *dest, size_t n)
{
    static const char *ranges[DF_FIELD_COUNT] = {
        "0.1 to 20", "a whole number from 1 to 1000", "0.5 to 20",
        "0 to 5", "0 to 1", "0 to 1"
    };

    snprintf(dest, n, "%s must be %s", g_dial_labels[id], ranges[id]);
}

/* Read all fields into jog. Returns -1 and names the bad field in status. */
static int dial_settings(struct Dial *d, JogSettings *jog, int report)
{
    double v[DF_FIELD_COUNT];
    int i;

    for (i = 0; i < DF_FIELD_COUNT; i++) {
        if (dial_parse(i, d->text[i], &v[i]) != 0) {
            if (report) {
                dial_range(i, d->status, sizeof d->status);
            }
            return -1;
        }
    }
    jog->turn_sec = v[DF_TURN];
    jog->clicks = (int)v[DF_CLICKS];
    jog->max_speed = v[DF_SPEED];
    jog->spinup_sec = v[DF_SPINUP];
    jog->grab_sec = v[DF_GRAB];
    jog->smooth_sec = v[DF_SMOOTH];
    return 0;
}

static void dial_apply(struct Dial *d)
{
    JogSettings jog;

    if (dial_settings(d, &jog, 1) == 0) {
        player_set_jog(&jog);
        if (strstr(d->status, " must be ")) {
            d->status[0] = '\0';
        }
    }
}

static int dial_changed(const struct Dial *d)
{
    int i;

    for (i = 0; i < DF_FIELD_COUNT; i++) {
        char a[32];
        char b[32];

        trim_copy(a, sizeof a, d->text[i]);
        trim_copy(b, sizeof b, d->saved_text[i]);
        if (strcmp(a, b) != 0) {
            return 1;
        }
    }
    return 0;
}

static void dial_layout(struct Dial *d, int rows, int cols)
{
    int i;
    int c;

    d->too_small = rows < DIAL_MIN_ROWS || cols < DIAL_MIN_COLS;
    for (i = 0; i < DF_FIELD_COUNT; i++) {
        d->field_r[i] = 3 + i;
    }
    d->box_c = 3 + DIAL_LABEL_W;
    d->button_r = 14;
    c = 3;
    for (i = 0; i < DF_COUNT - DF_FIELD_COUNT; i++) {
        d->button_c[i] = c;
        /* Leave room for Test to become Stop test. */
        c += (int)strlen(i + DF_FIELD_COUNT == DF_TEST ? "[ Stop test ]" : g_dial_buttons[i]) + 2;
    }
}

static void dial_line(FILE *fp, int row, int width, const char *text)
{
    cup(fp, row, 3);
    fprintf(fp, "%.*s", width, text);
}

static void dial_draw(struct Dial *d, FILE *fp, int rows, int cols)
{
    int width = cols - 4;
    char line[512];
    int i;

    fputs("\033[0m\033[2J", fp);
    if (d->too_small) {
        cup(fp, 1, 1);
        fprintf(fp, "Make the window at least %dx%d.", DIAL_MIN_COLS, DIAL_MIN_ROWS);
        return;
    }
    draw_hbar(fp, 1, 1, cols, " autodj dial ");
    for (i = 0; i < DF_FIELD_COUNT; i++) {
        int help_c = d->box_c + DIAL_BOX_W + 2;
        int help_w = cols - help_c - 1;

        cup(fp, d->field_r[i], 3);
        fputs(g_dial_labels[i], fp);
        draw_text_box(fp, d->field_r[i], d->box_c, DIAL_BOX_W, d->text[i],
                      d->focus == i ? d->cursor : 0, d->focus == i, 1);
        if (help_w > 4) {
            cup(fp, d->field_r[i], help_c);
            fprintf(fp, "\033[2m%.*s\033[0m", help_w, g_dial_help[i]);
        }
    }
    if (d->kfd_n == 0) {
        snprintf(line, sizeof line, "Dial    not available");
    } else if (d->gap_us > 0 && d->gap_us < 10000000LL) {
        snprintf(line, sizeof line, "Dial    %ld clicks, net %+ld, last gap %lld ms",
                 d->clicks, d->net, d->gap_us / 1000);
    } else {
        snprintf(line, sizeof line, "Dial    %ld clicks, net %+ld", d->clicks, d->net);
    }
    dial_line(fp, 10, width, line);
    if (d->testing) {
        int jogging = 0;
        double speed = player_speed(&jogging);
        char name[256];

        snprintf(line, sizeof line, "Speed   %+.2fx%s", speed, jogging ? "  (dial)" : "");
        dial_line(fp, 11, width, line);
        player_now_playing(name, sizeof name);
        snprintf(line, sizeof line, "Song    %s", name[0] ? name : "(loading)");
        dial_line(fp, 12, width, line);
    } else {
        dial_line(fp, 11, width, "Speed   test not running");
    }
    for (i = 0; i < DF_COUNT - DF_FIELD_COUNT; i++) {
        const char *label = g_dial_buttons[i];

        if (i + DF_FIELD_COUNT == DF_TEST && d->testing) {
            label = "[ Stop test ]";
        }
        draw_button(fp, d->button_r, d->button_c[i], label, d->focus == i + DF_FIELD_COUNT);
    }
    if (d->step == DIAL_STEP_TURN) {
        snprintf(line, sizeof line,
                 "Step 1 of 2: turn the dial exactly one full turn, then press Enter. "
                 "Clicks so far: %ld. Esc cancels.", labs(d->step_net));
    } else if (d->step == DIAL_STEP_SPIN_WAIT) {
        snprintf(line, sizeof line,
                 "Step 2 of 2: spin the dial as fast as you can, for 3 seconds. "
                 "Timing starts at the first click. Esc cancels.");
    } else if (d->step == DIAL_STEP_SPIN) {
        long long left = DIAL_SPIN_US - (now_us() - d->spin_start_us);

        snprintf(line, sizeof line, "Step 2 of 2: keep spinning... %.1f s", left > 0 ? left / 1e6 : 0.0);
    } else {
        snprintf(line, sizeof line, "%s", d->status);
    }
    {
        char lines[3][96];
        int n = wrap_lines(line, width < 95 ? width : 95, lines, 3);

        for (i = 0; i < n; i++) {
            dial_line(fp, 16 + i, width, lines[i]);
        }
    }
    dial_line(fp, rows - 1, width,
              "Tab/arrows move, Enter presses, Esc closes. Edits apply to the test at once.");
    draw_hbar(fp, rows, 1, cols, NULL);
    if (d->focus < DF_FIELD_COUNT) {
        fputs("\033[?25l", fp);
    }
}

static int dial_render(struct Dial *d)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *fp;
    int rows = 24;
    int cols = 80;
    size_t off = 0;
    int rc = 0;

    tty_size(g_tty, &rows, &cols);
    dial_layout(d, rows, cols);
    fp = open_memstream(&buf, &n);
    if (!fp) {
        return -1;
    }
    dial_draw(d, fp, rows, cols);
    if (fclose(fp) != 0) {
        free(buf);
        return -1;
    }
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
    free(buf);
    return rc;
}

static void dial_set_focus(struct Dial *d, int id)
{
    d->focus = (id % DF_COUNT + DF_COUNT) % DF_COUNT;
    d->cursor = d->focus < DF_FIELD_COUNT ? (int)strlen(d->text[d->focus]) : 0;
    d->confirm_close = 0;
}

static void dial_test_stop(struct Dial *d)
{
    if (!d->testing) {
        return;
    }
    player_stop();
    audio_close();
    d->testing = 0;
}

static void dial_test_start(struct Dial *d)
{
    PlayerSettings settings;
    JogSettings jog;
    unsigned rate = 44100;

    if (dial_settings(d, &jog, 1) != 0) {
        return;
    }
    if (!d->pl) {
        d->pl = playlist_load(d->cfg.folder, 1);
        if (!d->pl) {
            snprintf(d->status, sizeof d->status, "No songs found in %.200s", d->cfg.folder);
            return;
        }
    }
    if (!d->decoding) {
        if (decode_init() != 0) {
            snprintf(d->status, sizeof d->status, "Cannot start the MP3 decoder.");
            return;
        }
        d->decoding = 1;
    }
    if (audio_open(&rate) != 0) {
        snprintf(d->status, sizeof d->status,
                 "Cannot open the audio output. Is autodj or another player using it?");
        return;
    }
    memset(&settings, 0, sizeof settings);
    settings.overlap_sec = d->cfg.overlap_sec;
    settings.pause_sec = d->cfg.pause_sec;
    settings.resume_sec = d->cfg.resume_sec;
    settings.trim = d->cfg.trim_silence;
    settings.loudness = d->cfg.loudness;
    settings.loudness_lufs = d->cfg.loudness_lufs;
    settings.rate = rate;
    player_set_volume_percent(d->cfg.volume);
    player_set_jog(&jog);
    if (player_start(&settings, d->pl) != 0) {
        audio_close();
        snprintf(d->status, sizeof d->status, "Cannot start playback.");
        return;
    }
    d->testing = 1;
    snprintf(d->status, sizeof d->status,
             "Playing a random song. Turn the dial to scratch; let go to play on.");
}

static void dial_spin_done(struct Dial *d)
{
    double fastest = 0;
    double per_click;
    double speed;
    char text[16];
    int i;

    d->step = DIAL_STEP_NONE;
    for (i = 0; i + DIAL_SPIN_WINDOW < d->spin_n; i++) {
        long long span = d->spin_times[i + DIAL_SPIN_WINDOW] - d->spin_times[i];

        if (span > 0) {
            double rate = DIAL_SPIN_WINDOW * 1e6 / (double)span;

            if (rate > fastest) {
                fastest = rate;
            }
        }
    }
    if (fastest <= 0) {
        snprintf(d->status, sizeof d->status,
                 "Clicks per turn set to %s. Too few clicks to measure the spin; "
                 "Max speed is unchanged.", d->text[DF_CLICKS]);
        dial_apply(d);
        return;
    }
    {
        double turn = 0;
        double clicks = 0;

        if (dial_parse(DF_TURN, d->text[DF_TURN], &turn) != 0 ||
            dial_parse(DF_CLICKS, d->text[DF_CLICKS], &clicks) != 0) {
            dial_range(dial_parse(DF_TURN, d->text[DF_TURN], &turn) != 0 ? DF_TURN : DF_CLICKS,
                       d->status, sizeof d->status);
            return;
        }
        per_click = turn / clicks;
    }
    speed = fastest * per_click * 1.25;
    speed = floor(speed * 10.0 + 0.5) / 10.0;
    if (speed < JOG_SPEED_MIN) {
        speed = JOG_SPEED_MIN;
    }
    if (speed > JOG_SPEED_MAX) {
        speed = JOG_SPEED_MAX;
    }
    snprintf(text, sizeof text, "%.1f", speed);
    snprintf(d->text[DF_SPEED], sizeof d->text[DF_SPEED], "%s", text);
    snprintf(d->status, sizeof d->status,
             "Done. %s clicks per turn. Fastest spin %.0f clicks/s (%.1fx), so Max speed is %s. "
             "Save to keep it.", d->text[DF_CLICKS], fastest, fastest * per_click, text);
    dial_apply(d);
}

static void dial_turn_done(struct Dial *d)
{
    long n = labs(d->step_net);

    if (n < JOG_CLICKS_MIN || n > JOG_CLICKS_MAX) {
        d->step = DIAL_STEP_NONE;
        snprintf(d->status, sizeof d->status,
                 "No clicks counted. Calibrate again and turn the dial before Enter.");
        return;
    }
    snprintf(d->text[DF_CLICKS], sizeof d->text[DF_CLICKS], "%ld", n);
    d->step = DIAL_STEP_SPIN_WAIT;
    d->spin_n = 0;
}

static void dial_save(struct Dial *d)
{
    Config cfg;
    JogSettings jog;
    char err[512];

    if (dial_settings(d, &jog, 1) != 0) {
        return;
    }
    if (config_load(d->path, &cfg) != 0) {
        snprintf(d->status, sizeof d->status, "Cannot read %.200s. Fix it with autodj --config.", d->path);
        return;
    }
    cfg.jog_turn_sec = jog.turn_sec;
    cfg.jog_clicks = jog.clicks;
    cfg.jog_max_speed = jog.max_speed;
    cfg.jog_spinup_sec = jog.spinup_sec;
    cfg.jog_grab_sec = jog.grab_sec;
    cfg.jog_smooth_sec = jog.smooth_sec;
    if (config_save_jog(d->path, &cfg, err, sizeof err) != 0) {
        snprintf(d->status, sizeof d->status, "Save failed: %.200s", err);
        return;
    }
    memcpy(d->saved_text, d->text, sizeof d->text);
    snprintf(d->status, sizeof d->status,
             "Saved. Restart autodj to use the new dial settings.");
}

static int dial_press(struct Dial *d, int id)
{
    switch (id) {
    case DF_CALIBRATE:
        if (d->kfd_n == 0) {
            snprintf(d->status, sizeof d->status, "The dial is not available.");
            return 0;
        }
        d->step = DIAL_STEP_TURN;
        d->step_net = 0;
        return 0;
    case DF_TEST:
        if (d->testing) {
            dial_test_stop(d);
            snprintf(d->status, sizeof d->status, "Test stopped.");
        } else {
            dial_test_start(d);
        }
        return 0;
    case DF_SAVE:
        dial_save(d);
        return 0;
    case DF_CLOSE:
        if (dial_changed(d) && !d->confirm_close) {
            d->confirm_close = 1;
            snprintf(d->status, sizeof d->status,
                     "There are unsaved changes. Close again to discard them.");
            return 0;
        }
        return 1;
    default:
        dial_set_focus(d, id + 1);
        return 0;
    }
}

static int dial_hit(const struct Dial *d, int x, int y)
{
    int i;

    for (i = 0; i < DF_FIELD_COUNT; i++) {
        if (y == d->field_r[i] && x >= d->box_c && x < d->box_c + DIAL_BOX_W) {
            return i;
        }
    }
    for (i = 0; i < DF_COUNT - DF_FIELD_COUNT; i++) {
        const char *label = g_dial_buttons[i];

        if (i + DF_FIELD_COUNT == DF_TEST && d->testing) {
            label = "[ Stop test ]";
        }
        if (y == d->button_r && x >= d->button_c[i] &&
            x < d->button_c[i] + (int)strlen(label)) {
            return i + DF_FIELD_COUNT;
        }
    }
    return -1;
}

/* Returns 1 to close the view. */
static int dial_handle(struct Dial *d, const struct Event *ev)
{
    char *text = d->focus < DF_FIELD_COUNT ? d->text[d->focus] : NULL;
    int keep_confirm = 0;
    int rc = 0;

    if (ev->kind == EV_QUIT) {
        return 1;
    }
    if (d->step != DIAL_STEP_NONE) {
        if (ev->kind == EV_ESC) {
            d->step = DIAL_STEP_NONE;
            snprintf(d->status, sizeof d->status, "Calibration cancelled.");
        } else if (ev->kind == EV_ENTER && d->step == DIAL_STEP_TURN) {
            dial_turn_done(d);
        }
        return 0;
    }
    switch (ev->kind) {
    case EV_ESC:
        rc = dial_press(d, DF_CLOSE);
        keep_confirm = 1;
        break;
    case EV_TAB:
    case EV_DOWN:
        dial_set_focus(d, d->focus + 1);
        break;
    case EV_STAB:
    case EV_UP:
        dial_set_focus(d, d->focus - 1);
        break;
    case EV_LEFT:
        if (text && d->cursor > 0) {
            d->cursor--;
        } else if (!text) {
            dial_set_focus(d, d->focus - 1);
        }
        break;
    case EV_RIGHT:
        if (text && d->cursor < (int)strlen(text)) {
            d->cursor++;
        } else if (!text) {
            dial_set_focus(d, d->focus + 1);
        }
        break;
    case EV_HOME:
        d->cursor = 0;
        break;
    case EV_END:
        d->cursor = text ? (int)strlen(text) : 0;
        break;
    case EV_BACKSPACE:
        if (text) {
            text_backspace(text, &d->cursor);
            dial_apply(d);
        }
        break;
    case EV_DELETE:
        if (text) {
            text_delete(text, &d->cursor);
            dial_apply(d);
        }
        break;
    case EV_CHAR:
        if (text && (isdigit((unsigned char)ev->text[0]) || ev->text[0] == '.')) {
            text_insert(text, sizeof d->text[0], &d->cursor, ev->text);
            dial_apply(d);
        }
        break;
    case EV_ENTER:
        rc = dial_press(d, d->focus);
        keep_confirm = d->focus == DF_CLOSE;
        break;
    case EV_MOUSE: {
        int id = dial_hit(d, ev->x, ev->y);

        if (id >= 0) {
            int was_close = d->focus == DF_CLOSE && d->confirm_close;

            dial_set_focus(d, id);
            if (id >= DF_FIELD_COUNT) {
                d->confirm_close = was_close;
                rc = dial_press(d, id);
                keep_confirm = id == DF_CLOSE;
            }
        }
        break;
    }
    default:
        break;
    }
    if (!keep_confirm) {
        d->confirm_close = 0;
    }
    return rc;
}

/* Wait for a terminal event, dial clicks, or a redraw tick. */
static int dial_wait(struct Dial *d, struct Event *ev, int timeout_ms)
{
    struct pollfd pfds[9];
    int i;
    int pr;

    memset(ev, 0, sizeof *ev);
    if (g_winch) {
        g_winch = 0;
        ev->kind = EV_RESIZE;
        return 0;
    }
    if (g_unget >= 0) {
        unsigned char c = (unsigned char)g_unget;

        g_unget = -1;
        return event_from_byte(ev, c);
    }
    pfds[0].fd = g_tty;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    for (i = 0; i < d->kfd_n; i++) {
        pfds[1 + i].fd = d->kfd[i];
        pfds[1 + i].events = POLLIN;
        pfds[1 + i].revents = 0;
    }
    pr = poll(pfds, (nfds_t)(1 + d->kfd_n), timeout_ms);
    if (pr < 0) {
        if (errno != EINTR) {
            ev->kind = EV_QUIT;
        }
        return 0;
    }
    if (pr == 0) {
        return 0;
    }
    if (d->kfd_n > 0) {
        keys_poll(&d->cfg);
        d->kfd_n = keys_fds(d->kfd, 8);
        if (d->kfd_n == 0) {
            snprintf(d->status, sizeof d->status, "The remote was disconnected.");
        }
    }
    if (pfds[0].revents & POLLIN) {
        unsigned char c;
        ssize_t n = read(g_tty, &c, 1);

        if (n == 1) {
            return event_from_byte(ev, c);
        }
        if (n < 0 && errno == EINTR) {
            return 0;
        }
        ev->kind = EV_QUIT;
    } else if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        ev->kind = EV_QUIT;
    }
    return 0;
}

static void dial_text(const Config *cfg, char text[DF_FIELD_COUNT][16])
{
    snprintf(text[DF_TURN], 16, "%g", cfg->jog_turn_sec);
    snprintf(text[DF_CLICKS], 16, "%d", cfg->jog_clicks);
    snprintf(text[DF_SPEED], 16, "%g", cfg->jog_max_speed);
    snprintf(text[DF_SPINUP], 16, "%g", cfg->jog_spinup_sec);
    snprintf(text[DF_GRAB], 16, "%g", cfg->jog_grab_sec);
    snprintf(text[DF_SMOOTH], 16, "%g", cfg->jog_smooth_sec);
}

int dial_calibrate(const char *path)
{
    static struct Dial d;
    int saved_err = -1;
    int null_fd;
    int rc = 0;

    if (!path || !path[0]) {
        log_error("autodj", "config path is missing");
        return -1;
    }
    memset(&d, 0, sizeof d);
    d.path = path;
    if (config_load(path, &d.cfg) != 0) {
        return -1;
    }
    dial_text(&d.cfg, d.text);
    memcpy(d.saved_text, d.text, sizeof d.text);
    d.cursor = (int)strlen(d.text[0]);

    /* Player and key logs would scribble over the screen. */
    fflush(stderr);
    null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
        saved_err = dup(STDERR_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }

    if (keys_open() == 0) {
        d.kfd_n = keys_fds(d.kfd, 8);
    }
    if (d.kfd_n == 0) {
        snprintf(d.status, sizeof d.status,
                 "No shortcut remote or keypad found. Settings can still be edited and saved.");
    } else if (keys_grabbed() < d.kfd_n) {
        snprintf(d.status, sizeof d.status,
                 "The remote is busy. Stop autodj (it holds the remote), then run --calibrate again.");
        keys_close();
        d.kfd_n = 0;
    } else if (d.cfg.key_dial_fwd < 0 || d.cfg.key_dial_back < 0) {
        snprintf(d.status, sizeof d.status,
                 "Dial forward and Dial back are not bound. Set them with autodj --config.");
    } else {
        snprintf(d.status, sizeof d.status,
                 "Press Calibrate to measure the dial, or Test to try it on a random song.");
    }
    g_dial = &d;
    keys_set_jog_hook(dial_hook);
    g_unget = -1;
    g_winch = 0;
    if (enter_tty() != 0) {
        rc = -1;
        goto done;
    }
    for (;;) {
        struct Event ev;
        int busy = d.testing || d.step != DIAL_STEP_NONE;

        if (d.step == DIAL_STEP_SPIN && now_us() - d.spin_start_us >= DIAL_SPIN_US) {
            dial_spin_done(&d);
        }
        if (d.testing && !player_alive()) {
            dial_test_stop(&d);
            snprintf(d.status, sizeof d.status, "Playback stopped.");
        }
        if (dial_render(&d) != 0) {
            rc = -1;
            break;
        }
        dial_wait(&d, &ev, busy ? 100 : 500);
        if (dial_handle(&d, &ev)) {
            break;
        }
    }
    leave_tty();
done:
    keys_set_jog_hook(NULL);
    g_dial = NULL;
    dial_test_stop(&d);
    keys_close();
    if (d.decoding) {
        decode_shutdown();
    }
    if (d.pl) {
        playlist_free(d.pl);
    }
    if (saved_err >= 0) {
        fflush(stderr);
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
    }
    if (rc != 0) {
        log_error("autodj", "cannot draw the dial screen");
    }
    return rc;
}

static int dialog_button_self_test(void)
{
    struct Ui ui;
    struct Editor ed;
    struct Event ev;
    char *buf = NULL;
    size_t n = 0;
    FILE *fp;
    int code = 0;

    memset(&ui, 0, sizeof ui);
    ui.rows = 24;
    ui.cols = 80;
    fp = open_memstream(&buf, &n);
    if (!fp) {
        log_error("cfgedit", "cannot draw the key prompt");
        return -1;
    }
    draw_dialog(fp, &ui, "Select a key", "Press a new key, or Delete to remove KEY_KP0.",
                "[ Cancel ]", "[ Delete ]", 0);
    if (fclose(fp) != 0) {
        free(buf);
        log_error("cfgedit", "cannot finish the key prompt");
        return -1;
    }
    if (!buf || !strstr(buf, "[ Delete ]") || !strstr(buf, "\033[7m[ Cancel ]") ||
        strstr(buf, "\033[7m[ Delete ]") != NULL) {
        log_error("cfgedit", "the key prompt did not show Delete and Cancel");
        free(buf);
        return -1;
    }
    free(buf);
    buf = NULL;
    n = 0;
    if (!ui.del_on || !ui.ok_on || ui.del_r != ui.ok_r ||
        ui.del_w != 10 || ui.ok_w != 10 || ui.del_c + ui.del_w >= ui.ok_c) {
        log_error("cfgedit", "Delete and Cancel overlap");
        return -1;
    }
    if (!hit_del(&ui, ui.del_c, ui.del_r) ||
        !hit_del(&ui, ui.del_c + ui.del_w - 1, ui.del_r) ||
        hit_del(&ui, ui.del_c + ui.del_w, ui.del_r) ||
        hit_ok(&ui, ui.del_c, ui.del_r)) {
        log_error("cfgedit", "a click on Delete missed the button");
        return -1;
    }
    if (!hit_ok(&ui, ui.ok_c, ui.ok_r) ||
        !hit_ok(&ui, ui.ok_c + ui.ok_w - 1, ui.ok_r) ||
        hit_ok(&ui, ui.ok_c - 1, ui.ok_r) ||
        hit_del(&ui, ui.ok_c, ui.ok_r)) {
        log_error("cfgedit", "a click on Cancel missed the button");
        return -1;
    }

    memset(&ui, 0, sizeof ui);
    ui.rows = 24;
    ui.cols = 80;
    fp = open_memstream(&buf, &n);
    if (!fp) {
        log_error("cfgedit", "cannot draw the message dialog");
        return -1;
    }
    draw_dialog(fp, &ui, "Cannot save", "Folder is required.", "[ OK ]", NULL, 0);
    fclose(fp);
    free(buf);
    if (ui.del_on || !ui.ok_on || !hit_ok(&ui, ui.ok_c, ui.ok_r)) {
        log_error("cfgedit", "the message dialog grew a Delete button");
        return -1;
    }

    if (g_key_n != 0) {
        log_error("cfgedit", "key capture was already open");
        return -1;
    }
    memset(&ed, 0, sizeof ed);
    snprintf(ed.edit.key_skip, sizeof ed.edit.key_skip, "KEY_KPDOT");
    snprintf(ed.edit.key_pause, sizeof ed.edit.key_pause, "KEY_KP0");
    ed.capture = 1;
    ed.capture_id = FE_KEY_SKIP;
    ed.capture_btn = CAP_DELETE;
    ed.ui = ui;
    ed.ui.del_on = 1;
    ed.ui.del_r = 10;
    ed.ui.del_c = 20;
    ed.ui.del_w = 10;
    ed.ui.ok_on = 1;
    ed.ui.ok_r = 10;
    ed.ui.ok_c = 32;
    ed.ui.ok_w = 10;
    memset(&ev, 0, sizeof ev);
    ev.kind = EV_MOUSE;
    ev.x = 20;
    ev.y = 10;
    if (handle_event(&ed, &ev) != 0 || ed.capture != 0 || ed.edit.key_skip[0] != '\0' ||
        strcmp(ed.edit.key_pause, "KEY_KP0") != 0) {
        log_error("cfgedit", "Delete did not remove the current key");
        return -1;
    }

    snprintf(ed.edit.key_skip, sizeof ed.edit.key_skip, "KEY_KPDOT");
    ed.capture = 1;
    ed.capture_id = FE_KEY_SKIP;
    ed.capture_btn = CAP_CANCEL;
    memset(&ev, 0, sizeof ev);
    ev.kind = EV_MOUSE;
    ev.x = 32;
    ev.y = 10;
    if (handle_event(&ed, &ev) != 0 || ed.capture != 0 ||
        strcmp(ed.edit.key_skip, "KEY_KPDOT") != 0) {
        log_error("cfgedit", "Cancel removed the current key");
        return -1;
    }

    ed.capture = 1;
    ed.capture_btn = CAP_DELETE;
    memset(&ev, 0, sizeof ev);
    ev.kind = EV_ENTER;
    if (handle_event(&ed, &ev) != 0 || ed.edit.key_skip[0] != '\0') {
        log_error("cfgedit", "Enter on Delete left the key in place");
        return -1;
    }

    snprintf(ed.edit.key_skip, sizeof ed.edit.key_skip, "KEY_KPDOT");
    ed.capture = 1;
    ed.capture_btn = CAP_CANCEL;
    memset(&ev, 0, sizeof ev);
    ev.kind = EV_LEFT;
    if (handle_event(&ed, &ev) != 0 || ed.capture != 1 || ed.capture_btn != CAP_DELETE ||
        strcmp(ed.edit.key_skip, "KEY_KPDOT") != 0) {
        log_error("cfgedit", "Left did not move to Delete");
        return -1;
    }
    ev.kind = EV_ESC;
    if (handle_event(&ed, &ev) != 0 || ed.capture != 0 ||
        strcmp(ed.edit.key_skip, "KEY_KPDOT") != 0) {
        log_error("cfgedit", "Esc removed the current key");
        return -1;
    }

    snprintf(ed.edit.key_skip, sizeof ed.edit.key_skip, "KEY_KPDOT");
    ed.capture = 1;
    ed.capture_id = FE_KEY_SKIP;
    if (key_from_name("KEY_A", &code) != 0) {
        log_error("cfgedit", "KEY_A lookup failed");
        return -1;
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = EV_HWKEY;
    ev.code = code;
    if (handle_event(&ed, &ev) != 0 || ed.capture != 0 ||
        strcmp(ed.edit.key_skip, "KEY_A") != 0) {
        log_error("cfgedit", "a new key was not stored");
        return -1;
    }
    return 0;
}

static int render_tab(struct Editor *ed, int rows, int cols, int tab, char **buf, size_t *n)
{
    FILE *fp;

    memset(ed, 0, sizeof *ed);
    ed->path = "master.cfg";
    ed->tab = tab;
    layout_ui(&ed->ui, &ed->edit, rows, cols, tab);
    if (ed->ui.too_small) {
        log_error("cfgedit", "layout %dx%d was too small", cols, rows);
        return -1;
    }
    fp = open_memstream(buf, n);
    if (!fp) {
        log_error("cfgedit", "could not capture the screen");
        return -1;
    }
    draw_screen(ed, fp);
    fclose(fp);
    return 0;
}

static int screen_self_test(void)
{
    struct Editor ed;
    char *buf = NULL;
    size_t n = 0;

    if (render_tab(&ed, 24, 100, TAB_BUTTONS, &buf, &n) != 0) {
        return -1;
    }
    if (ed.ui.field_r[FE_KEY_CHORUS] < 0 ||
        ed.ui.field_r[FE_KEY_FLANGER] <= ed.ui.field_r[FE_KEY_PHASER] ||
        ed.ui.field_r[FE_KEY_EFFECTS_OFF] <= ed.ui.field_r[FE_KEY_FLANGER] ||
        ed.ui.tab_y[TAB_CHORUS] != ed.ui.tab_y[TAB_MAIN] ||
        ed.ui.tab_y[TAB_FLANGER] != ed.ui.tab_y[TAB_MAIN] ||
        !buf || strstr(buf, "[ Chorus ]") == NULL || strstr(buf, "[ Flanger ]") == NULL ||
        strstr(buf, "Chorus") == NULL || strstr(buf, "Flanger") == NULL ||
        strstr(buf, "All effects off") == NULL) {
        log_error("cfgedit", "wide Buttons screen is missing chorus and flanger");
        free(buf);
        return -1;
    }
    if (hit_tab(&ed.ui, ed.ui.tab_c[TAB_CHORUS], ed.ui.tab_y[TAB_CHORUS]) != TAB_CHORUS ||
        hit_tab(&ed.ui, ed.ui.tab_c[TAB_FLANGER], ed.ui.tab_y[TAB_FLANGER]) != TAB_FLANGER) {
        log_error("cfgedit", "chorus or flanger tab could not be clicked");
        free(buf);
        return -1;
    }
    free(buf);
    buf = NULL;
    n = 0;

    if (render_tab(&ed, 24, 80, TAB_BUTTONS, &buf, &n) != 0) {
        return -1;
    }
    if (ed.ui.tab_y[TAB_PHASER] != ed.ui.tab_y[TAB_MAIN] ||
        ed.ui.tab_y[TAB_CHORUS] != ed.ui.tab_y[TAB_MAIN] + 1 ||
        ed.ui.tab_y[TAB_FLANGER] != ed.ui.tab_y[TAB_CHORUS]) {
        log_error("cfgedit", "narrow window did not wrap chorus and flanger");
        free(buf);
        return -1;
    }
    free(buf);
    buf = NULL;
    n = 0;

    if (render_tab(&ed, 24, 100, TAB_CHORUS, &buf, &n) != 0) {
        return -1;
    }
    if (ed.ui.field_r[FE_CH_DELAY] < 0 || ed.ui.field_r[FE_CH_VOICES] < 0 ||
        !buf || strstr(buf, "Delay ms") == NULL || strstr(buf, "Voices") == NULL ||
        strstr(buf, "drifting in pitch") == NULL) {
        log_error("cfgedit", "Chorus screen is missing its settings");
        free(buf);
        return -1;
    }
    free(buf);
    buf = NULL;
    n = 0;

    if (render_tab(&ed, 24, 100, TAB_FLANGER, &buf, &n) != 0) {
        return -1;
    }
    if (ed.ui.field_r[FE_FL_DELAY] < 0 || ed.ui.field_r[FE_FL_FEEDBACK] < 0 ||
        !buf || strstr(buf, "Delay ms") == NULL || strstr(buf, "Feedback") == NULL ||
        strstr(buf, "jet-like whoosh") == NULL) {
        log_error("cfgedit", "Flanger screen is missing its settings");
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

int cfgedit_self_test(void)
{
    ConfigEdit edit;
    const char *who = "x";

    if (dialog_button_self_test() != 0) {
        return -1;
    }
    if (screen_self_test() != 0) {
        return -1;
    }
    memset(&edit, 0, sizeof edit);
    snprintf(edit.key_pause, sizeof edit.key_pause, "KEY_KP0");
    snprintf(edit.key_skip, sizeof edit.key_skip, "KEY_KPDOT");
    snprintf(edit.key_reverb, sizeof edit.key_reverb, " KEY_KPPLUS ");
    if (key_conflict(&edit, FE_KEY_SKIP, "KEY_KP0", &who) != 1 ||
        !who || strcmp(who, "Pause/resume") != 0) {
        log_error("cfgedit", "duplicate pause key was not reported");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_PAUSE, "KEY_KP0", &who) != 0 || !who || who[0] != '\0') {
        log_error("cfgedit", "the same field was treated as a duplicate");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_AUTOPAN, "KEY_KPPLUS", &who) != 1 ||
        !who || strcmp(who, "Reverb") != 0) {
        log_error("cfgedit", "trimmed duplicate was not reported");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_AUTOPAN, "KEY_KPASTERISK", &who) != 0) {
        log_error("cfgedit", "a free key was rejected");
        return -1;
    }
    if (key_conflict(&edit, FE_KEY_AUTOPAN, "", &who) != 0) {
        log_error("cfgedit", "an empty key was treated as a duplicate");
        return -1;
    }
    snprintf(edit.key_rew05, sizeof edit.key_rew05, "KEY_KP1");
    snprintf(edit.key_rew10, sizeof edit.key_rew10, "KEY_KP1");
    if (key_conflict(&edit, FE_KEY_REW10, "KEY_KP1", &who) != 1 ||
        !who || strcmp(who, "Rewind 0.5s") != 0) {
        log_error("cfgedit", "duplicate rewind key was not reported");
        return -1;
    }
    return 0;
}
