/*
 * NOTES module: markdown file browser, editor and preview, with the Obsidian
 * vault sync panel.
 *
 *   browser   folders then notes newest first; tap or Enter opens
 *   document  EDIT (textarea) or VIEW (rendered markdown)
 *   sync      Self-hosted LiveSync settings, SYNC NOW, RESCAN, the log
 *
 * Keys in the browser: Enter open, N new, R rename, D delete, del up a
 * folder (or back to the deck at the top), S the sync panel in the vault.
 * Keys in a document: Alt+V edit / view, Alt+E save, Esc (SYM+0) or del
 * in VIEW closes. Autosaves every 20 s while there are changes.
 */
#include "app_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_md.h"
#include "deck_modal.h"
#include "deck_shell.h"
#include "deck_sync.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "notes_store.h"

#define MAX_NOTES 128
#define AUTOSAVE_MS 20000

#define HID_D 0x07
#define HID_E 0x08
#define HID_N 0x11
#define HID_R 0x15
#define HID_S 0x16
#define HID_V 0x19

typedef enum { MODE_EDIT, MODE_VIEW } mode_t_;

static struct {
    lv_obj_t *parent; /* module content area */
    lv_obj_t *screen; /* current browser, document or sync root */

    /* browser */
    note_entry_t notes[MAX_NOTES];
    int count;

    /* sync panel */
    bool sync_open;
    lv_obj_t *sync_status;
    lv_obj_t *sync_log;
    lv_obj_t *sync_btn;
    lv_timer_t *sync_timer;
    sync_state_t sync_seen;

    /* document */
    bool doc_open;
    char name[NOTES_NAME_MAX];
    mode_t_ mode;
    bool dirty;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *mode_btn[2];
    lv_obj_t *ta;
    lv_obj_t *preview;
    lv_obj_t *osk;
    lv_timer_t *autosave;
    uint32_t status_until;
    uint32_t dirty_since;
} s_n DECK_PSRAM_BSS;

static void show_browser(const char *focus_name);
static void open_doc(const char *name, mode_t_ mode);
static void show_sync(void);

/* ---- Small helpers ------------------------------------------------------- */

static lv_obj_t *page(void)
{
    /* Usually called from a click inside the old page, so delete it later. */
    if (s_n.screen) {
        lv_obj_add_flag(s_n.screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_n.screen);
    }
    lv_group_remove_all_objs(deck_input_group());
    s_n.screen = deck_box(s_n.parent);
    lv_obj_set_size(s_n.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_n.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_n.screen, 6, 0);
    lv_obj_set_style_pad_ver(s_n.screen, 4, 0);
    lv_obj_set_style_pad_row(s_n.screen, 4, 0);
    return s_n.screen;
}

/* A button that keyboard focus skips (keys have shortcuts instead). */
static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, g_font.mono_s);
    lv_obj_set_style_min_height(b, 20, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *toolbar(lv_obj_t *root)
{
    lv_obj_t *bar = deck_box(root);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    return bar;
}

static void fmt_size(char *buf, size_t n, long size)
{
    if (size < 1024) {
        snprintf(buf, n, "%ldB", size);
    } else {
        snprintf(buf, n, "%.1fK", (double)size / 1024.0);
    }
}

/* ---- Browser actions ----------------------------------------------------- */

static void new_named(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL) return;
    char name[NOTES_NAME_MAX];
    if (!notes_sanitize(text, name, sizeof(name))) return;
    if (!notes_exists(name)) {
        char body[128];
        char title[NOTES_NAME_MAX];
        snprintf(title, sizeof(title), "%s", name);
        title[strlen(title) - 3] = '\0'; /* drop .md */
        int len = snprintf(body, sizeof(body), "# %s\n\n", title);
        notes_write(name, body, (size_t)len);
    }
    open_doc(name, MODE_EDIT);
}

static void new_note(void) { deck_modal_prompt("NEW NOTE", "", new_named, NULL); }

static char s_pending[NOTES_NAME_MAX];

static void rename_named(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL) return;
    char to[NOTES_NAME_MAX];
    if (notes_sanitize(text, to, sizeof(to)) && strcmp(to, s_pending) != 0 && notes_rename(s_pending, to)) {
        show_browser(to);
    } else {
        show_browser(s_pending);
    }
}

static void rename_note(const char *name)
{
    snprintf(s_pending, sizeof(s_pending), "%s", name);
    char initial[NOTES_NAME_MAX];
    snprintf(initial, sizeof(initial), "%s", name);
    initial[strlen(initial) - 3] = '\0';
    deck_modal_prompt("RENAME", initial, rename_named, NULL);
}

static void delete_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) notes_delete(s_pending);
    show_browser(NULL);
}

static void delete_note(const char *name)
{
    snprintf(s_pending, sizeof(s_pending), "%s", name);
    char msg[128];
    snprintf(msg, sizeof(msg), "Permanently delete %s?", name);
    deck_modal_confirm("DELETE", msg, "DELETE", delete_confirmed, NULL);
}

/* The focused row's note, or NULL when a folder or nothing is focused. */
static const char *focused_note(void)
{
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    if (f == NULL || s_n.doc_open) return NULL;
    intptr_t i = (intptr_t)lv_obj_get_user_data(f);
    return (i >= 1 && i <= s_n.count && !s_n.notes[i - 1].is_dir) ? s_n.notes[i - 1].name : NULL;
}

static void row_clicked(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (s_n.notes[i - 1].is_dir) {
        if (notes_enter(s_n.notes[i - 1].name)) show_browser(NULL);
        return;
    }
    open_doc(s_n.notes[i - 1].name, MODE_VIEW);
}

static void go_up(void)
{
    char was[NOTES_NAME_MAX] = "";
    const char *slash        = strrchr(notes_subdir(), '/');
    snprintf(was, sizeof(was), "%s", slash ? slash + 1 : notes_subdir());
    if (notes_up()) show_browser(was[0] ? was : NULL);
}

static void source_clicked(lv_event_t *e)
{
    (void)e;
    notes_set_vault_mode(!notes_vault_mode());
    show_browser(NULL);
}

static void sync_clicked(lv_event_t *e)
{
    (void)e;
    show_sync();
}

static void new_clicked(lv_event_t *e)
{
    (void)e;
    new_note();
}

static void up_clicked(lv_event_t *e)
{
    (void)e;
    go_up();
}

static void sync_refresh(lv_timer_t *t);

/* Half-second tick: the sync panel redraws its status and log; the browser
 * reloads itself when a sync that was running finishes. */
static void browser_sync_tick(lv_timer_t *t)
{
    (void)t;
    if (s_n.sync_open) {
        sync_refresh(NULL);
        return;
    }
    sync_state_t st = deck_sync_state();
    if (st != s_n.sync_seen) {
        s_n.sync_seen = st;
        if (st == SYNC_DONE && !s_n.doc_open) show_browser(NULL);
    }
}

/* ---- Sync panel ---------------------------------------------------------- */

static void setting_entered(const char *text, void *ud)
{
    const char *key = (const char *)ud;
    if (text == NULL) return;
    hal_cfg_set_str(key, text[0] ? text : NULL);
    if (strcmp(key, SYNC_KEY_PHRASE) == 0) hal_cfg_set_str("ls_mkey", NULL); /* cached key is for the old passphrase */
    show_sync();
}

static void edit_setting(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    char cur[160]   = "";
    hal_cfg_get_str(key, cur, sizeof(cur));
    if (strcmp(key, SYNC_KEY_PASS) == 0) {
        deck_modal_password("COUCHDB PASSWORD", setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_PHRASE) == 0) {
        deck_modal_password("E2EE PASSPHRASE (EMPTY = OFF)", setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_URL) == 0) {
        deck_modal_prompt("SERVER (http://host:5984)", cur, setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_DB) == 0) {
        deck_modal_prompt("DATABASE", cur, setting_entered, (void *)key);
    } else {
        deck_modal_prompt("USER", cur, setting_entered, (void *)key);
    }
}

static const int s_auto_opts[] = {0, 5, 15, 60};

static void auto_clicked(lv_event_t *e)
{
    int cur = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0), next = 0;
    for (int i = 0; i < 4; i++) {
        if (s_auto_opts[i] == cur) next = s_auto_opts[(i + 1) % 4];
    }
    hal_cfg_set_i32(SYNC_KEY_AUTO, next);
    char b[16];
    snprintf(b, sizeof(b), next ? "%d MIN" : "OFF", next);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0), b);
}

static void sync_now_clicked(lv_event_t *e)
{
    (void)e;
    if (deck_sync_state() == SYNC_RUNNING) {
        deck_sync_cancel();
    } else if (!deck_sync_configured()) {
        if (s_n.sync_status) {
            lv_label_set_text(s_n.sync_status, "SET SERVER AND DATABASE");
            lv_obj_set_style_text_color(s_n.sync_status, g_pal.warn, 0);
        }
    } else {
        deck_sync_start();
    }
}

static void sync_back_clicked(lv_event_t *e)
{
    (void)e;
    show_browser(NULL);
}

static void rescan_clicked(lv_event_t *e)
{
    (void)e;
    if (deck_sync_state() != SYNC_RUNNING && deck_sync_configured()) deck_sync_start_full();
}

static void sync_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_n.sync_open) return;
    sync_state_t st = deck_sync_state();
    const char *txt;
    lv_color_t col = g_pal.dim;
    char buf[64];
    switch (st) {
        case SYNC_RUNNING:
            txt = "SYNCING...";
            col = g_pal.accent;
            break;
        case SYNC_DONE: {
            long age = deck_sync_age();
            snprintf(buf, sizeof(buf), age < 90 ? "SYNCED JUST NOW" : "SYNCED %ld MIN AGO", age / 60);
            txt = buf;
            col = g_pal.ok;
            break;
        }
        case SYNC_FAILED:
            txt = "SYNC FAILED";
            col = g_pal.danger;
            break;
        default: txt = deck_sync_configured() ? "READY" : "NOT CONFIGURED"; break;
    }
    lv_label_set_text(s_n.sync_status, txt);
    lv_obj_set_style_text_color(s_n.sync_status, col, 0);
    lv_label_set_text(lv_obj_get_child(s_n.sync_btn, 0), st == SYNC_RUNNING ? "STOP" : "SYNC");

    static char lines[24][96]; /* off the UI task's stack */
    int n = deck_sync_log(lines, 24);
    static char all[24 * 100];
    size_t o = 0;
    for (int i = 0; i < n && o < sizeof(all) - 100; i++) {
        o += (size_t)snprintf(all + o, sizeof(all) - o, "%s> %s", i ? "\n" : "", lines[i]);
    }
    lv_label_set_text(s_n.sync_log, n ? all : "> no sync yet this boot");
    lv_obj_scroll_to_y(lv_obj_get_parent(s_n.sync_log), LV_COORD_MAX, LV_ANIM_OFF);
}

static lv_obj_t *sync_row(lv_obj_t *parent, const char *key, const char *label, bool secret)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 22);
    deck_label(row, g_font.mono_s, g_pal.dim, label);
    char cur[160] = "";
    hal_cfg_get_str(key, cur, sizeof(cur));
    lv_obj_t *val = deck_label(row, g_font.mono_s, cur[0] ? g_pal.text : g_pal.warn,
                               cur[0] ? (secret ? "********" : cur) : "NOT SET");
    lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(val, 150);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 72, 0);
    lv_obj_t *b = tool_button(row, "EDIT", edit_setting, (void *)key);
    lv_group_add_obj(deck_input_group(), b); /* the panel is driven by its buttons */
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    return row;
}

static void show_sync(void)
{
    s_n.doc_open   = false;
    s_n.sync_open  = true;
    lv_obj_t *root = page();

    lv_obj_t *head = toolbar(root);
    lv_obj_t *t    = deck_label(head, g_font.disp_s, g_pal.accent, "// VAULT SYNC");
    lv_obj_set_flex_grow(t, 1);
    s_n.sync_btn = tool_button(head, "SYNC", sync_now_clicked, NULL);
    lv_group_add_obj(deck_input_group(), s_n.sync_btn);
    lv_obj_t *rescan = tool_button(head, "RESCAN", rescan_clicked, NULL);
    lv_group_add_obj(deck_input_group(), rescan);
    tool_button(head, ICON_CHEVRON_LEFT, sync_back_clicked, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(head, 3), 0), g_font.icon_s, 0);

    s_n.sync_status = deck_label(root, g_font.mono_m, g_pal.dim, "");

    lv_obj_t *col = deck_box(root);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

    sync_row(col, SYNC_KEY_URL, "SERVER", false);
    sync_row(col, SYNC_KEY_DB, "DATABASE", false);
    sync_row(col, SYNC_KEY_USER, "USER", false);
    sync_row(col, SYNC_KEY_PASS, "PASSWORD", true);
    sync_row(col, SYNC_KEY_PHRASE, "PASSPHRASE", true);
    lv_obj_t *arow = deck_box(col);
    lv_obj_set_width(arow, LV_PCT(100));
    lv_obj_set_height(arow, 22);
    deck_label(arow, g_font.mono_s, g_pal.dim, "AUTO SYNC");
    int auto_min = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0);
    char ab[16];
    snprintf(ab, sizeof(ab), auto_min ? "%d MIN" : "OFF", auto_min);
    lv_obj_t *autob = tool_button(arow, ab, auto_clicked, NULL);
    lv_group_add_obj(deck_input_group(), autob);
    lv_obj_set_width(autob, 52);
    lv_obj_align(autob, LV_ALIGN_RIGHT_MID, 0, 0);

    deck_section(col, "LOG");
    lv_obj_t *logbox = deck_box(col);
    lv_obj_set_width(logbox, LV_PCT(100));
    lv_obj_set_height(logbox, 70);
    lv_obj_add_flag(logbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(logbox, LV_DIR_VER);
    s_n.sync_log = deck_label(logbox, g_font.mono_s, g_pal.text, "");
    lv_obj_set_width(s_n.sync_log, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_n.sync_log, 1, 0);

    deck_label(root, g_font.mono_s, g_pal.dim, "[S] SYNC   [DEL] BACK   Notes sync both ways; attachments stay in Obsidian.");
    lv_label_set_long_mode(lv_obj_get_child(root, (int32_t)lv_obj_get_child_count(root) - 1), LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(lv_obj_get_child(root, (int32_t)lv_obj_get_child_count(root) - 1), LV_PCT(100));
    sync_refresh(NULL);
    lv_group_focus_obj(s_n.sync_btn);
}

/* ---- Browser ------------------------------------------------------------- */

static void show_browser(const char *focus_name)
{
    s_n.doc_open   = false;
    s_n.sync_open  = false;
    lv_obj_t *root = page();

    const char *dir = notes_dir();
    s_n.count       = dir ? notes_list(s_n.notes, MAX_NOTES) : 0;
    bool vault      = notes_vault_mode();

    lv_obj_t *head = toolbar(root);
    if (notes_subdir()[0]) {
        lv_obj_t *up = tool_button(head, ICON_CHEVRON_LEFT, up_clicked, NULL);
        lv_obj_set_style_text_font(lv_obj_get_child(up, 0), g_font.icon_s, 0);
    }
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, vault ? "// VAULT" : "// NOTES");
    lv_obj_t *where = deck_label(head, g_font.mono_s, g_pal.dim, "");
    if (dir == NULL) {
        lv_label_set_text(where, "NO STORAGE");
    } else if (notes_subdir()[0]) {
        const char *slash = strrchr(notes_subdir(), '/');
        lv_label_set_text(where, slash ? slash + 1 : notes_subdir()); /* the folder we are in */
    } else {
        lv_label_set_text(where, hal_storage_is_sd() ? "SD" : "FLASH");
    }
    lv_label_set_long_mode(where, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(where, 1);
    (void)t;
    if (vault) tool_button(head, deck_sync_state() == SYNC_RUNNING ? "SYNCING" : "SYNC", sync_clicked, NULL);
    tool_button(head, vault ? "VAULT" : "LOCAL", source_clicked, NULL);
    tool_button(head, "+", new_clicked, NULL);

    lv_obj_t *list = deck_box(root);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_style_pad_all(list, 3, 0); /* room for the focus glow */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *focus = NULL;
    if (dir == NULL) {
        lv_obj_t *l = deck_label(list, g_font.mono_s, g_pal.warn, "No SD card and no internal storage.");
        lv_obj_set_width(l, LV_PCT(100));
    } else if (s_n.count == 0 && vault && notes_subdir()[0] == '\0') {
        lv_obj_t *l = deck_label(list, g_font.mono_s, g_pal.dim, "Empty vault. SYNC pulls your notes in; LOCAL switches back.");
        lv_obj_set_width(l, LV_PCT(100));
    } else if (s_n.count == 0) {
        deck_label(list, g_font.mono_s, g_pal.dim, "Empty. N or + starts a note.");
    }
    for (int i = 0; i < s_n.count; i++) {
        const note_entry_t *n = &s_n.notes[i];
        lv_obj_t *row         = deck_panel(list, DECK_CUT_BR, 6);
        lv_obj_set_size(row, LV_PCT(100), 24);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_group_add_obj(deck_input_group(), row);

        lv_obj_t *nm = deck_icon_text(row, n->is_dir ? ICON_FOLDER : ICON_MARKDOWN, n->name, g_font.mono_s,
                                      n->is_dir ? g_pal.accent : g_pal.text);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *txt = lv_obj_get_child(nm, 1);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(txt, 220);

        char meta[24];
        if (n->is_dir) {
            snprintf(meta, sizeof(meta), "DIR");
        } else {
            fmt_size(meta, sizeof(meta), n->size);
        }
        lv_obj_t *m = deck_label(row, g_font.mono_s, g_pal.dim, meta);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);

        if (focus == NULL || (focus_name && strcmp(focus_name, n->name) == 0)) focus = row;
    }

    lv_obj_t *hint = deck_label(root, g_font.mono_s, g_pal.dim,
                                vault ? "[ENTER] OPEN  [N] NEW  [R] RENAME  [D] DELETE  [S] SYNC"
                                      : "[ENTER] OPEN  [N] NEW  [R] RENAME  [D] DELETE");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(hint, LV_PCT(100));

    if (focus) {
        lv_group_focus_obj(focus);
        lv_obj_add_state(focus, LV_STATE_FOCUS_KEY); /* show it; LVGL only sets this for real key focus */
    }
}

/* ---- Document ------------------------------------------------------------ */

static void flash_status(const char *text, lv_color_t color)
{
    lv_label_set_text(s_n.status, text);
    lv_obj_set_style_text_color(s_n.status, color, 0);
    s_n.status_until = lv_tick_get() + 1500;
}

static void update_title(void)
{
    lv_label_set_text_fmt(s_n.title, "%s%s", s_n.name, s_n.dirty ? " *" : "");
}

static bool save(void)
{
    if (!s_n.doc_open || !s_n.dirty) return true;
    const char *txt = lv_textarea_get_text(s_n.ta);
    bool ok         = notes_write(s_n.name, txt, strlen(txt));
    if (ok) s_n.dirty = false;
    update_title();
    flash_status(ok ? "SAVED" : "SAVE FAILED", ok ? g_pal.ok : g_pal.danger);
    return ok;
}

static void render_preview(void)
{
    if (s_n.preview == NULL) return;
    const char *txt = lv_textarea_get_text(s_n.ta);
    deck_md_render(s_n.preview, txt, strlen(txt));
}

static void apply_mode(mode_t_ mode)
{
    s_n.mode  = mode;
    bool edit = mode == MODE_EDIT;

    if (edit) {
        lv_obj_remove_flag(s_n.ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_n.preview, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_n.ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_n.preview, LV_OBJ_FLAG_HIDDEN);
        render_preview();
    }
    if (s_n.osk) {
        if (edit) {
            lv_obj_remove_flag(s_n.osk, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_n.osk, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (i == (int)mode) {
            lv_obj_add_state(s_n.mode_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_n.mode_btn[i], LV_STATE_CHECKED);
        }
    }
    if (edit) {
        lv_group_focus_obj(s_n.ta);
    } else {
        lv_group_focus_obj(NULL); /* del then means close, not delete */
    }
}

static void close_doc(void)
{
    char name[NOTES_NAME_MAX];
    snprintf(name, sizeof(name), "%s", s_n.name);
    save();
    if (s_n.autosave) lv_timer_delete(s_n.autosave);
    s_n.autosave = NULL;
    s_n.ta = s_n.preview = s_n.osk = NULL;
    show_browser(name);
}

static void ta_changed(lv_event_t *e)
{
    (void)e;
    if (!s_n.dirty) {
        s_n.dirty       = true;
        s_n.dirty_since = lv_tick_get();
        update_title();
    }
}

static void autosave_tick(lv_timer_t *t)
{
    (void)t;
    if (s_n.dirty && lv_tick_elaps(s_n.dirty_since) >= AUTOSAVE_MS) save();
    if (s_n.status_until && lv_tick_get() > s_n.status_until) {
        lv_label_set_text(s_n.status, "");
        s_n.status_until = 0;
    }
}

static void mode_clicked(lv_event_t *e) { apply_mode((mode_t_)(intptr_t)lv_event_get_user_data(e)); }
static void save_clicked(lv_event_t *e)
{
    (void)e;
    s_n.dirty = true; /* explicit save always writes */
    save();
}
static void close_clicked(lv_event_t *e)
{
    (void)e;
    close_doc();
}

static void open_doc(const char *name, mode_t_ mode)
{
    size_t len = 0;
    char *text = notes_read(name, &len);
    if (text == NULL) {
        show_browser(NULL);
        return;
    }

    s_n.doc_open = true;
    s_n.dirty    = false;
    snprintf(s_n.name, sizeof(s_n.name), "%s", name);
    lv_obj_t *root = page();

    /* Toolbar: name, status, EDIT / VIEW, save, close. */
    lv_obj_t *bar = toolbar(root);
    s_n.title     = deck_label(bar, g_font.mono_s, g_pal.text, "");
    lv_label_set_long_mode(s_n.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s_n.title, 1);
    s_n.status = deck_label(bar, g_font.mono_s, g_pal.ok, "");
    static const char *labels[2] = {"EDIT", "VIEW"};
    for (int i = 0; i < 2; i++) {
        s_n.mode_btn[i] = tool_button(bar, labels[i], mode_clicked, (void *)(intptr_t)i);
    }
    tool_button(bar, "SAVE", save_clicked, NULL);
    lv_obj_t *x = tool_button(bar, "X", close_clicked, NULL);
    (void)x;
    update_title();

    /* Body: editor or preview, mode decides which. */
    lv_obj_t *body = deck_box(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);

    s_n.ta = lv_textarea_create(body);
    lv_obj_set_size(s_n.ta, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_font(s_n.ta, g_font.mono_m, 0);
    lv_obj_set_style_text_color(s_n.ta, g_pal.text, 0);
    lv_obj_set_style_text_line_space(s_n.ta, 2, 0);
    lv_obj_set_style_bg_color(s_n.ta, lv_color_hex(0x04050A), 0);
    lv_obj_set_style_border_color(s_n.ta, g_pal.line, 0);
    lv_obj_set_style_border_color(s_n.ta, g_pal.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_n.ta, 0, 0);
    lv_obj_set_style_pad_all(s_n.ta, 6, 0);
    lv_obj_set_style_bg_color(s_n.ta, g_pal.accent, LV_PART_CURSOR);
    lv_textarea_set_text(s_n.ta, text);
    lv_textarea_set_cursor_pos(s_n.ta, mode == MODE_EDIT ? LV_TEXTAREA_CURSOR_LAST : 0);
    lv_obj_add_event_cb(s_n.ta, ta_changed, LV_EVENT_VALUE_CHANGED, NULL);
    free(text);

    s_n.preview = deck_panel(body, DECK_CUT_TL | DECK_CUT_BR, 8);
    lv_obj_set_size(s_n.preview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_n.preview, 8, 0);
    lv_obj_add_flag(s_n.preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_n.preview, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_n.preview, LV_SCROLLBAR_MODE_AUTO);

    /* Touch-only (no keyboard attached): on-screen keyboard while editing. */
    s_n.osk = NULL;
    if (!hal_kbd_present()) {
        s_n.osk = lv_keyboard_create(root);
        lv_obj_set_size(s_n.osk, LV_PCT(100), LV_PCT(45));
        lv_keyboard_set_textarea(s_n.osk, s_n.ta);
        lv_group_remove_obj(s_n.osk);
    }

    lv_group_add_obj(deck_input_group(), s_n.ta);
    s_n.autosave     = lv_timer_create(autosave_tick, 500, NULL);
    s_n.status_until = 0;
    apply_mode(mode);
}

/* ---- Keys ---------------------------------------------------------------- */

static bool doc_key(const deck_key_t *k)
{
    bool chord = (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) != 0;
    if (chord && k->code == HID_E) {
        s_n.dirty = true;
        save();
        return true;
    }
    if (chord && k->code == HID_V) {
        apply_mode(s_n.mode == MODE_VIEW ? MODE_EDIT : MODE_VIEW);
        return true;
    }
    if (chord) return false;
    if (k->code == HID_ESC) {
        close_doc();
        return true;
    }
    if (s_n.mode == MODE_VIEW) {
        if (k->code == HID_BACKSPACE || k->code == HID_DELETE) {
            close_doc();
            return true;
        }
        /* Nothing is focused in the preview: arrows and space scroll it. */
        int32_t page = lv_obj_get_height(s_n.preview) - 30;
        int32_t dy   = k->code == HID_DOWN ? 30 : k->code == HID_UP ? -30 : k->code == HID_SPACE ? page : 0;
        if (dy) lv_obj_scroll_by_bounded(s_n.preview, 0, -dy, LV_ANIM_ON);
        return dy != 0;
    }
    if (k->code == HID_TAB) {
        lv_textarea_add_text(s_n.ta, "  "); /* indent instead of moving focus */
        return true;
    }
    return false;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (s_n.doc_open) return doc_key(k);
    bool chord = (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) != 0;
    if (chord) return false;
    if (s_n.sync_open) {
        if (k->code == HID_ESC || k->code == HID_BACKSPACE || k->code == HID_DELETE) {
            show_browser(NULL);
            return true;
        }
        if (k->code == HID_S) {
            sync_now_clicked(NULL);
            return true;
        }
        return false;
    }

    /* Browser: single letters act, nothing here takes text. */
    if (k->code == HID_N) {
        new_note();
        return true;
    }
    if (k->code == HID_S && notes_vault_mode()) {
        show_sync();
        return true;
    }
    if ((k->code == HID_BACKSPACE || k->code == HID_DELETE) && notes_subdir()[0]) {
        go_up();
        return true;
    }
    const char *f = focused_note();
    if (f && k->code == HID_R) {
        rename_note(f);
        return true;
    }
    if (f && k->code == HID_D) {
        delete_note(f);
        return true;
    }
    return false;
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_n, 0, sizeof(s_n));
    s_n.parent = parent;
    notes_seed();
    /* A stale vault syncs on its own when AUTO SYNC is set. */
    int auto_min = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0);
    if (notes_vault_mode() && auto_min > 0 && deck_sync_configured() && deck_sync_state() != SYNC_RUNNING) {
        long age = deck_sync_age();
        if (age < 0 || age > (long)auto_min * 60) deck_sync_start();
    }
    s_n.sync_seen  = deck_sync_state();
    s_n.sync_timer = lv_timer_create(browser_sync_tick, 500, NULL);
    show_browser(NULL);
    return true;
}

/* Leaving the module: the editor still exists here, so this is the last
 * chance to save. */
static void notes_exit(deck_app_t *self)
{
    (void)self;
    deck_modal_discard();
    if (s_n.doc_open) save();
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_n.autosave) lv_timer_delete(s_n.autosave);
    if (s_n.sync_timer) lv_timer_delete(s_n.sync_timer);
    memset(&s_n, 0, sizeof(s_n));
}

static deck_app_t s_app = {
    .name     = "NOTES",
    .tagline  = "MARKDOWN I/O",
    .icon     = ICON_MARKDOWN,
    .eta      = NULL,
    .hotkey   = HID_LETTER('N'),
    .on_start = start,
    .on_exit  = notes_exit,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_notes(void) { return &s_app; }
