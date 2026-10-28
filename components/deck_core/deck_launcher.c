/*
 * Home screen: a 3 x 3 grid of square tiles (icon, name, its Alt digit) and a
 * one-line system readout along the bottom. The grid is laid out by
 * deck_home.h; a group screen shows that group's modules the same way.
 */
#include "deck_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "deck_boot.h"
#include "deck_hal.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define COLS 3
#define TILE 60
#define GAP 6
#define MAX_TILES 9

static lv_obj_t *s_tiles[MAX_TILES];
static deck_tile_t s_items[MAX_TILES]; /* what each tile opens */
static int s_tile_count;
static deck_group_t *s_group;          /* NULL on the home screen */
static int s_sel_home;                 /* keyboard selection, survives trips into apps */
static int s_sel_group;
static deck_group_t *s_sel_group_of;   /* the group s_sel_group belongs to */
static bool s_kbd_nav;                 /* only show the selection once the keyboard is used */
static lv_obj_t *s_stats;
static lv_timer_t *s_stats_timer;
static uint32_t s_stats_start;
static bool s_stats_settled;

static int *sel(void) { return s_group ? &s_sel_group : &s_sel_home; }

static void open_item(const deck_tile_t *it)
{
    if (it->group) {
        deck_shell_open_group(it->group);
    } else if (it->app) {
        deck_shell_launch(it->app);
    }
}

static void tile_clicked(lv_event_t *e)
{
    int i  = (int)(intptr_t)lv_event_get_user_data(e);
    *sel() = i;
    open_item(&s_items[i]);
}

static void show_selection(void)
{
    for (int i = 0; i < s_tile_count; i++) {
        if (s_kbd_nav && i == *sel()) {
            lv_obj_add_state(s_tiles[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tiles[i], LV_STATE_CHECKED);
        }
    }
}

static void stats_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_shortcuts();
}

static lv_obj_t *build_tile(lv_obj_t *parent, const deck_tile_t *it, int index)
{
    deck_app_t *app   = it->app;
    deck_group_t *grp = it->group;
    bool offline      = app && app->eta != NULL;

    lv_obj_t *t = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 7);
    lv_obj_set_size(t, TILE, TILE);
    if (app == NULL && grp == NULL) {
        /* Empty slot: a dim outline, nothing inside, nothing to tap. */
        deck_panel_set_outline(t, g_pal.grid);
        deck_panel_set_fill(t, g_pal.bg);
        return t;
    }
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    deck_panel_set_hazard(t, offline);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    /* The Alt digit that opens it, tucked in the top right. */
    char num[4];
    snprintf(num, sizeof(num), "%d", index + 1);
    lv_obj_t *n = deck_label(t, g_font.mono_s, g_pal.accent2, num);
    lv_obj_align(n, LV_ALIGN_TOP_RIGHT, -3, 1);

    lv_obj_t *icon = deck_label(t, g_font.icon_l, offline ? g_pal.dim : g_pal.accent, grp ? grp->icon : app->icon);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *name = deck_label(t, g_font.mono_s, offline ? g_pal.dim : g_pal.text, grp ? grp->name : app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(name, TILE - 6);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);
    return t;
}

static void launcher_deleted(lv_event_t *e)
{
    (void)e;
    if (s_stats_timer) {
        lv_timer_delete(s_stats_timer);
        s_stats_timer = NULL;
    }
    s_tile_count = 0;
    s_stats      = NULL;
}

/* ---- Stats line ---------------------------------------------------------
 * On arrival the line "decodes": scrambled glyphs that lock in left to right
 * with a little color flicker, then it settles into a static readout
 * refreshed once a second. */

#define DECODE_MS 2000
#define DECODE_TICK_MS 40

static const char *stats_text(void)
{
    static char buf[96];
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    hal_power_t p;
    hal_power_read(&p);

    char net[24], pwr[16];
    if (net_state() == NET_CONNECTED) {
        snprintf(net, sizeof(net), "%s", net_ip());
    } else {
        snprintf(net, sizeof(net), "%s", net_state_name(net_state()));
    }
    if (!p.valid) {
        snprintf(pwr, sizeof(pwr), "--");
    } else if (p.percent < 0) {
        snprintf(pwr, sizeof(pwr), "USB");
    } else {
        snprintf(pwr, sizeof(pwr), "%.2fV", (double)p.volts);
    }
    snprintf(buf, sizeof(buf), "NET %s // RAM %luK // %s // %s", net,
             (unsigned long)(si.heap_int_free / 1024), hal_sd_mounted() ? "SD OK" : "NO SD", pwr);
    return buf;
}

static void stats_tick(lv_timer_t *t)
{
    const char *target = stats_text();
    if (s_stats_settled) {
        lv_label_set_text(s_stats, target);
        return;
    }

    /* The home screen is built behind the boot POST; hold the decode until
     * the POST is gone so it is actually seen. */
    if (deck_boot_active()) s_stats_start = lv_tick_get();

    uint32_t elapsed = lv_tick_elaps(s_stats_start);
    if (elapsed >= DECODE_MS) {
        s_stats_settled = true;
        lv_label_set_text(s_stats, target);
        lv_obj_set_style_text_color(s_stats, g_pal.dim, 0);
        lv_timer_set_period(t, 1000);
        return;
    }

    /* Characters lock in left to right over the last two thirds of the run;
     * before that everything is noise. */
    static const char noise[] = "!<>-_/\\[]{}=+*^?#%&0123456789ABCDEFXZ";
    size_t len      = strlen(target);
    int32_t lock_at = (int32_t)elapsed - DECODE_MS / 3;
    size_t locked   = lock_at <= 0 ? 0 : (size_t)((uint64_t)lock_at * len / (DECODE_MS * 2 / 3));
    char buf[96];
    for (size_t i = 0; i < len && i < sizeof(buf) - 1; i++) {
        char c = target[i];
        buf[i] = (i < locked || c == ' ') ? c : noise[lv_rand(0, sizeof(noise) - 2)];
    }
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    lv_label_set_text(s_stats, buf);

    bool tear = lv_rand(0, 9) < 3;
    static const lv_color_t *flicker[3];
    flicker[0] = &g_pal.accent;
    flicker[1] = &g_pal.accent2;
    flicker[2] = &g_pal.dim;
    lv_obj_set_style_text_color(s_stats, *flicker[tear ? lv_rand(0, 1) : 2], 0);
}

void deck_launcher_create(lv_obj_t *parent, deck_group_t *group)
{
    s_group = group;
    if (group != s_sel_group_of) {
        s_sel_group    = 0;
        s_sel_group_of = group;
    }

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(root, 5, 0);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_add_event_cb(root, launcher_deleted, LV_EVENT_DELETE, NULL);

    /* Tiles */
    lv_obj_t *grid = deck_box(root);
    lv_obj_set_width(grid, COLS * TILE + (COLS - 1) * GAP);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    s_tile_count = 0;
    if (group) {
        for (size_t i = 0; i < group->count && s_tile_count < MAX_TILES; i++) {
            s_items[s_tile_count++] = (deck_tile_t){.app = group->apps[i]};
        }
    } else {
        for (size_t i = 0; i < deck_home_count() && s_tile_count < MAX_TILES; i++) {
            s_items[s_tile_count++] = *deck_home_tile(i);
        }
    }
    for (int i = 0; i < s_tile_count; i++) {
        s_tiles[i] = build_tile(grid, &s_items[i], i);
    }
    if (*sel() >= s_tile_count) *sel() = 0;
    show_selection();

    /* Readout; tapping it opens the shortcut list. */
    s_stats = deck_label(root, g_font.mono_s, g_pal.dim, "");
    lv_label_set_long_mode(s_stats, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_stats, HAL_SCREEN_W - 12);
    lv_obj_set_style_text_align(s_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_stats, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_stats, 6);
    lv_obj_add_event_cb(s_stats, stats_clicked, LV_EVENT_CLICKED, NULL);
    s_stats_start   = lv_tick_get();
    s_stats_settled = group != NULL; /* the decode plays on the home screen only */
    s_stats_timer   = lv_timer_create(stats_tick, group ? 1000 : DECODE_TICK_MS, NULL);
    stats_tick(s_stats_timer);
}

bool deck_launcher_key(const deck_key_t *k)
{
    if (s_tile_count == 0) return false;

    int *s     = sel();
    int n      = s_tile_count;
    int target = -1;
    switch (k->code) {
        case HID_LEFT: *s = s_kbd_nav ? (*s + n - 1) % n : *s; break;
        case HID_RIGHT:
        case HID_TAB: *s = s_kbd_nav ? (*s + 1) % n : *s; break;
        case HID_UP: *s = s_kbd_nav ? (*s + n - COLS) % n : *s; break;
        case HID_DOWN: *s = s_kbd_nav ? (*s + COLS) % n : *s; break;
        case HID_ENTER:
        case HID_SPACE:
            if (s_kbd_nav) target = *s;
            break;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + n) {
                target = k->code - HID_1;
                *s     = target;
            } else {
                return false;
            }
            break;
    }
    s_kbd_nav = true;
    show_selection();
    if (target >= 0) open_item(&s_items[target]);
    return true;
}
