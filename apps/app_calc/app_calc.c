/*
 * CALC module: expression input with live result, a scrolling tape of past
 * results, a base readout (hex/oct/bin) for integers, and a touch keypad
 * with SCI and PROG pages that swaps in for the tape when wanted.
 *
 * Keys: Enter or = evaluate, Up/Down recall the tape, Esc clears the line
 * (or leaves when it is empty), Alt+D degrees/radians, Alt+K keypad,
 * Alt+L clear tape (Ctrl works too, for the simulator). Starting a line
 * with an operator continues from ans.
 */
#include "app_calc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calc_engine.h"
#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TAPE_MAX 100
#define HID_D 0x07
#define HID_K 0x0E
#define HID_L 0x0F

typedef struct {
    char expr[96];
    char result[48];
} tape_entry_t;

/* Session state: survives leaving and re-entering the module. */
static struct {
    tape_entry_t tape[TAPE_MAX];
    int count;
    calc_ctx_t ctx;
    bool prog_page;
    bool keypad_shown;
} s_c DECK_PSRAM_BSS; /* 14 KB tape */

/* UI handles, valid while the module is open. */
static struct {
    lv_obj_t *tape;
    lv_obj_t *tape_list;
    lv_obj_t *input;
    lv_obj_t *preview;
    lv_obj_t *bases;
    lv_obj_t *angle_chip;
    lv_obj_t *keypad_col;
    lv_obj_t *btnm;
    lv_obj_t *page_btn[2];
    int recall; /* -1 = editing a new line, else tape index being shown */
} s_ui;

/* ---- Keypad maps --------------------------------------------------------- */

static const char *s_sci_map[] = {
    "sin", "cos", "tan", "\xE2\x88\x9A", "\xCF\x80", "\n",
    "ln", "log", "^", "(", ")", "\n",
    "7", "8", "9", "\xC3\xB7", "DEL", "\n",
    "4", "5", "6", "\xC3\x97", "AC", "\n",
    "1", "2", "3", "\xE2\x88\x92", "ans", "\n",
    "0", ".", "!", "+", "=", "",
};

static const char *s_prog_map[] = {
    "A", "B", "C", "D", "E", "F", "\n",
    "0x", "0b", "&", "|", "xor", "~", "\n",
    "7", "8", "9", "<<", ">>", "DEL", "\n",
    "4", "5", "6", "\xC3\x97", "\xC3\xB7", "AC", "\n",
    "1", "2", "3", "+", "\xE2\x88\x92", "ans", "\n",
    "0", "(", ")", "%", "=", "",
};

/* What a keypad label types into the line. */
static const char *insert_text(const char *label)
{
    static const char *map[][2] = {
        {"sin", "sin("}, {"cos", "cos("}, {"tan", "tan("}, {"ln", "ln("}, {"log", "log("},
        {"xor", " xor "}, {"<<", " << "}, {">>", " >> "}, {"\xE2\x88\x92", "-"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(label, map[i][0])) return map[i][1];
    }
    return label;
}

/* ---- Evaluation ---------------------------------------------------------- */

static void update_bases(double v, bool valid)
{
    char hex[96], oct[96], bin[96]; /* one size: calc_format_bases uses n for all three */
    if (valid && calc_format_bases(v, hex, oct, bin, sizeof(bin))) {
        lv_label_set_text_fmt(s_ui.bases, "HEX %s  OCT %s  BIN %s", hex, oct, bin);
    } else {
        lv_label_set_text(s_ui.bases, "");
    }
}

static void update_preview(void)
{
    const char *txt = lv_textarea_get_text(s_ui.input);
    calc_result_t r = calc_eval(txt, &s_c.ctx);
    if (r.ok) {
        char buf[48];
        calc_format(r.value, buf, sizeof(buf));
        lv_label_set_text_fmt(s_ui.preview, "= %s", buf);
        lv_obj_set_style_text_color(s_ui.preview, g_pal.accent2, 0);
        update_bases(r.value, true);
    } else if (!strcmp(r.error, "empty")) {
        lv_label_set_text(s_ui.preview, "");
        update_bases(s_c.ctx.ans, s_c.count > 0);
    } else {
        lv_label_set_text_fmt(s_ui.preview, "! %s", r.error);
        lv_obj_set_style_text_color(s_ui.preview, strcmp(r.error, "incomplete") ? g_pal.danger : g_pal.dim, 0);
    }
}

static void add_tape_row(const tape_entry_t *e, bool scroll)
{
    lv_obj_t *row = deck_box(s_ui.tape_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *x = deck_label(row, g_font.mono_s, g_pal.dim, e->expr);
    lv_label_set_long_mode(x, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(x, LV_PCT(100), 0);
    lv_obj_t *r = deck_label(row, g_font.mono_m, g_pal.text, "");
    lv_label_set_text_fmt(r, "= %s", e->result);
    if (scroll) {
        lv_obj_update_layout(s_ui.tape_list);
        lv_obj_scroll_to_y(s_ui.tape_list, LV_COORD_MAX, LV_ANIM_ON);
    }
}

static void evaluate(void)
{
    const char *txt = lv_textarea_get_text(s_ui.input);
    calc_result_t r = calc_eval(txt, &s_c.ctx);
    if (!r.ok) {
        if (strcmp(r.error, "empty") != 0) {
            lv_textarea_set_cursor_pos(s_ui.input, r.pos);
            lv_label_set_text_fmt(s_ui.preview, "! %s", r.error);
            lv_obj_set_style_text_color(s_ui.preview, g_pal.danger, 0);
        }
        return;
    }
    if (s_c.count == 0) lv_obj_clean(s_ui.tape_list); /* drop the hint */
    if (s_c.count == TAPE_MAX) {
        memmove(&s_c.tape[0], &s_c.tape[1], sizeof(s_c.tape[0]) * (TAPE_MAX - 1));
        s_c.count--;
        lv_obj_delete(lv_obj_get_child(s_ui.tape_list, 0));
    }
    tape_entry_t *e = &s_c.tape[s_c.count++];
    snprintf(e->expr, sizeof(e->expr), "%s", txt);
    calc_format(r.value, e->result, sizeof(e->result));
    s_c.ctx.ans = r.value;

    add_tape_row(e, true);
    s_ui.recall = -1;
    lv_textarea_set_text(s_ui.input, "");
    update_preview();
}

static void recall(int dir)
{
    if (s_c.count == 0) return;
    int i = s_ui.recall < 0 ? (dir < 0 ? s_c.count - 1 : -1) : s_ui.recall + dir;
    if (i < 0 || i >= s_c.count) {
        s_ui.recall = -1;
        lv_textarea_set_text(s_ui.input, "");
    } else {
        s_ui.recall = i;
        lv_textarea_set_text(s_ui.input, s_c.tape[i].expr);
    }
    update_preview();
}

/* ---- Keypad -------------------------------------------------------------- */

static void style_keypad(lv_obj_t *m)
{
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 3, 0);
    lv_obj_set_style_bg_color(m, g_pal.panel_hi, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(m, g_pal.line, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, g_font.mono_m, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, g_pal.text, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, g_pal.accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(m, g_pal.bg, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(m, g_pal.accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, g_pal.bg, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(m, g_pal.accent, LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* Highlight "=" so the grid reads at a glance. */
static void decorate_keys(void)
{
    const char **map = (const char **)lv_buttonmatrix_get_map(s_ui.btnm);
    uint32_t id      = 0;
    for (int i = 0; map[i][0] != '\0'; i++) {
        if (!strcmp(map[i], "\n")) continue;
        if (!strcmp(map[i], "=")) lv_buttonmatrix_set_button_ctrl(s_ui.btnm, id, LV_BUTTONMATRIX_CTRL_CHECKED);
        id++;
    }
}

/* True if the cursor sits right after "0x" plus hex digits. */
static bool in_hex_literal(void)
{
    const char *t = lv_textarea_get_text(s_ui.input);
    int i         = (int)lv_textarea_get_cursor_pos(s_ui.input);
    while (i > 0 && strchr("0123456789abcdefABCDEF", t[i - 1])) i--;
    return i >= 2 && t[i - 1] == 'x' && t[i - 2] == '0';
}

static void keypad_clicked(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *label = lv_buttonmatrix_get_button_text(m, id);
    if (label == NULL) return;

    if (!strcmp(label, "=")) {
        evaluate();
    } else if (!strcmp(label, "AC")) {
        lv_textarea_set_text(s_ui.input, "");
    } else if (!strcmp(label, "DEL")) {
        lv_textarea_delete_char(s_ui.input);
    } else if (label[1] == '\0' && label[0] >= 'A' && label[0] <= 'F' && !in_hex_literal()) {
        lv_textarea_add_text(s_ui.input, "0x"); /* a hex digit starts a hex number */
        lv_textarea_add_text(s_ui.input, label);
    } else {
        lv_textarea_add_text(s_ui.input, insert_text(label));
    }
    s_ui.recall = -1;
}

static void set_page(bool prog)
{
    s_c.prog_page = prog;
    lv_buttonmatrix_set_map(s_ui.btnm, prog ? s_prog_map : s_sci_map);
    decorate_keys();
    for (int i = 0; i < 2; i++) {
        if (i == (int)prog) {
            lv_obj_add_state(s_ui.page_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_ui.page_btn[i], LV_STATE_CHECKED);
        }
    }
}

/* The keypad and the tape share the space above the input line. */
static void set_keypad_shown(bool shown)
{
    s_c.keypad_shown = shown;
    if (shown) {
        lv_obj_remove_flag(s_ui.keypad_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.tape, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.keypad_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.tape, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_angle_chip(void)
{
    lv_label_set_text(lv_obj_get_child(s_ui.angle_chip, 0), s_c.ctx.degrees ? "DEG" : "RAD");
}

static void page_clicked(lv_event_t *e) { set_page((bool)(intptr_t)lv_event_get_user_data(e)); }
static void angle_clicked(lv_event_t *e)
{
    (void)e;
    s_c.ctx.degrees = !s_c.ctx.degrees;
    update_angle_chip();
    update_preview();
}
static void keypad_toggle_clicked(lv_event_t *e)
{
    (void)e;
    set_keypad_shown(!s_c.keypad_shown);
}
static void input_changed(lv_event_t *e)
{
    (void)e;
    update_preview();
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, g_font.mono_s);
    lv_obj_set_style_min_height(b, 20, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.recall = -1;

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(root, 6, 0);
    lv_obj_set_style_pad_ver(root, 4, 0);
    lv_obj_set_style_pad_row(root, 4, 0);

    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// TAPE");
    lv_obj_set_flex_grow(t, 1);
    s_ui.page_btn[0] = small_button(head, "SCI", page_clicked, (void *)0);
    s_ui.page_btn[1] = small_button(head, "PROG", page_clicked, (void *)1);
    s_ui.angle_chip  = small_button(head, "RAD", angle_clicked, NULL);
    lv_obj_t *kb     = small_button(head, ICON_KEYPAD, keypad_toggle_clicked, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(kb, 0), g_font.icon_s, 0);

    /* Tape and keypad share this slot. */
    s_ui.tape = deck_panel(root, DECK_CUT_TL, 8);
    lv_obj_set_width(s_ui.tape, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.tape, 1);
    lv_obj_set_style_pad_all(s_ui.tape, 5, 0);
    s_ui.tape_list = deck_box(s_ui.tape);
    lv_obj_set_size(s_ui.tape_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ui.tape_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.tape_list, 3, 0);
    lv_obj_add_flag(s_ui.tape_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.tape_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.tape_list, LV_SCROLLBAR_MODE_AUTO);
    if (s_c.count == 0) {
        lv_obj_t *hint = deck_label(s_ui.tape_list, g_font.mono_s, g_pal.dim,
                                    "Type an expression, Enter evaluates.\nUp/Down recall. Alt+D deg/rad.");
        lv_obj_set_width(hint, LV_PCT(100));
    }
    for (int i = 0; i < s_c.count; i++) add_tape_row(&s_c.tape[i], false);
    lv_obj_update_layout(s_ui.tape_list);
    lv_obj_scroll_to_y(s_ui.tape_list, LV_COORD_MAX, LV_ANIM_OFF);

    s_ui.keypad_col = deck_box(root);
    lv_obj_set_width(s_ui.keypad_col, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.keypad_col, 1);
    s_ui.btnm = lv_buttonmatrix_create(s_ui.keypad_col);
    lv_obj_set_size(s_ui.btnm, LV_PCT(100), LV_PCT(100));
    style_keypad(s_ui.btnm);
    lv_obj_add_event_cb(s_ui.btnm, keypad_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_remove_obj(s_ui.btnm); /* keyboard focus stays in the input line */

    lv_obj_t *in = deck_panel(root, DECK_CUT_BR, 8);
    lv_obj_set_size(in, LV_PCT(100), LV_SIZE_CONTENT);
    deck_panel_set_outline(in, g_pal.accent);
    lv_obj_set_style_pad_all(in, 5, 0);
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(in, 1, 0);

    s_ui.input = lv_textarea_create(in);
    lv_textarea_set_one_line(s_ui.input, true);
    lv_textarea_set_placeholder_text(s_ui.input, "0");
    lv_obj_set_width(s_ui.input, LV_PCT(100));
    lv_obj_set_style_text_font(s_ui.input, g_font.mono_m, 0);
    lv_obj_set_style_text_color(s_ui.input, g_pal.text, 0);
    lv_obj_set_style_bg_opa(s_ui.input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.input, 0, 0);
    lv_obj_set_style_pad_all(s_ui.input, 0, 0);
    lv_obj_set_style_bg_color(s_ui.input, g_pal.accent, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_ui.input, input_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_ui.preview = deck_label(in, g_font.mono_m, g_pal.accent2, "");
    lv_obj_set_width(s_ui.preview, LV_PCT(100));
    lv_obj_set_style_text_align(s_ui.preview, LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.bases = deck_label(in, g_font.mono_s, g_pal.dim, "");
    lv_label_set_long_mode(s_ui.bases, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_ui.bases, LV_PCT(100));

    set_page(s_c.prog_page);
    update_angle_chip();
    /* Keypad starts out on a touch-only deck, or if you last left it open. */
    set_keypad_shown(s_c.keypad_shown || !hal_kbd_present());

    lv_group_add_obj(deck_input_group(), s_ui.input);
    lv_group_focus_obj(s_ui.input);
    update_preview();
    return true;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    bool chord = (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) != 0;
    if (chord && k->code == HID_D) {
        angle_clicked(NULL);
        return true;
    }
    if (chord && k->code == HID_K) {
        set_keypad_shown(!s_c.keypad_shown);
        return true;
    }
    if (chord && k->code == HID_L) {
        s_c.count = 0;
        lv_obj_clean(s_ui.tape_list);
        update_preview();
        return true;
    }
    if (chord) return false;
    if (k->code == HID_ENTER || k->key == '=') {
        evaluate();
        return true;
    }
    if (k->code == HID_UP || k->code == HID_DOWN) {
        recall(k->code == HID_UP ? -1 : 1);
        return true;
    }
    if (k->code == HID_ESC && lv_textarea_get_text(s_ui.input)[0] != '\0') {
        lv_textarea_set_text(s_ui.input, "");
        s_ui.recall = -1;
        return true;
    }

    /* Typing on the keyboard: the keypad is just in the way now. */
    if (k->key >= 0x20 && k->key < 0x7F && s_c.keypad_shown) set_keypad_shown(false);

    /* An operator on an empty line continues from the last result. */
    if (lv_textarea_get_text(s_ui.input)[0] == '\0' && s_c.count > 0 && k->key && strchr("+*/^%&|", (int)k->key)) {
        lv_textarea_add_text(s_ui.input, "ans");
    }
    s_ui.recall = -1;
    return false;
}

static void stop(deck_app_t *self)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
}

static deck_app_t s_app = {
    .name     = "CALC",
    .tagline  = "NUMERIC CORE",
    .icon     = ICON_CALCULATOR,
    .eta      = NULL,
    .hotkey   = HID_LETTER('C'),
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_calc(void) { return &s_app; }
