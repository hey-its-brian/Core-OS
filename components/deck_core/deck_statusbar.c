/*
 * HUD status bar (22 px): brand or a back chevron and the module name on the
 * left, the clock in the middle, hardware indicators on the right.
 * Refreshes once a second.
 */
#include "deck_statusbar.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_portal.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_update.h"
#include "deck_widgets.h"

static lv_obj_t *s_kbd, *s_sd, *s_net, *s_upd, *s_link;

static lv_obj_t *s_bar;
static lv_obj_t *s_left;
static lv_obj_t *s_clock;
static lv_obj_t *s_right;
static lv_obj_t *s_pwr; /* icon + percent */
static lv_timer_t *s_timer;

static void back_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_back();
}

static void clock_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_sleep();
}

static void build_left(deck_group_t *group, deck_app_t *app)
{
    lv_obj_clean(s_left);
    if (app == NULL && group == NULL) {
        lv_obj_t *brand = deck_box(s_left);
        lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
        deck_label(brand, g_font.disp_s, g_pal.text, "CORE");
        deck_label(brand, g_font.disp_s, g_pal.accent, "//OS");
        return;
    }
    /* A chevron that goes back one level, then where we are. */
    lv_obj_t *back = deck_label(s_left, g_font.icon_s, g_pal.accent, ICON_CHEVRON_LEFT);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *name = deck_label(s_left, g_font.disp_s, g_pal.text, app ? app->name : group->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_max_width(name, 96, 0);
}

static void set_icon(lv_obj_t *l, const char *icon, lv_color_t color)
{
    lv_label_set_text(l, icon);
    lv_obj_set_style_text_color(l, color, 0);
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);

    bool kbd = hal_kbd_present();
    set_icon(s_kbd, kbd ? ICON_KEYBOARD : ICON_KEYBOARD_OFF, kbd ? g_pal.accent : g_pal.dim);
    set_icon(s_sd, ICON_SD, hal_sd_mounted() ? g_pal.accent : g_pal.dim);
    deck_update_tick(); /* startup and hourly update checks, and the popup */
    deck_saver_tick();  /* screen timeout */
    if (ota_state() == OTA_AVAILABLE) {
        lv_obj_remove_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
        set_icon(s_upd, ICON_BOLT, (tm.tm_sec & 1) ? g_pal.accent2 : g_pal.dim);
    } else {
        lv_obj_add_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
    }

    switch (net_state()) {
        case NET_CONNECTED: set_icon(s_net, ICON_WIFI, g_pal.accent); break;
        case NET_CONNECTING:
        case NET_STARTING: set_icon(s_net, ICON_WIFI, (tm.tm_sec & 1) ? g_pal.warn : g_pal.dim); break;
        default: set_icon(s_net, ICON_WIFI_OFF, g_pal.dim); break;
    }

    /* The portal outlives its module, so its idle shutdown is driven here. */
    portal_tick();
    if (portal_running()) {
        lv_obj_remove_flag(s_link, LV_OBJ_FLAG_HIDDEN);
        set_icon(s_link, ICON_SERVER, portal_busy() && (tm.tm_sec & 1) ? g_pal.text : g_pal.accent2);
    } else {
        lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
    }

    hal_power_t p;
    hal_power_read(&p);
    char buf[16];
    if (!p.valid) {
        deck_icon_text_set(s_pwr, ICON_BATTERY_EMPTY, "", g_pal.dim);
    } else if (p.percent < 0) {
        deck_icon_text_set(s_pwr, ICON_PLUG, "", g_pal.accent);
    } else {
        snprintf(buf, sizeof(buf), "%d", p.percent);
        deck_icon_text_set(s_pwr, p.charging ? ICON_BATTERY_CHG : ICON_BATTERY, buf,
                           p.percent <= 15 && !p.charging ? g_pal.danger : g_pal.accent);
    }
}

static lv_obj_t *indicator(lv_obj_t *parent)
{
    return deck_label(parent, g_font.icon_s, g_pal.dim, "");
}

void deck_statusbar_create(lv_obj_t *parent)
{
    s_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_bar);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar, LV_PCT(100), DECK_STATUSBAR_H);
    lv_obj_set_style_bg_color(s_bar, g_pal.panel, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);

    /* Accent rule along the bottom with a brighter segment on the left. */
    lv_obj_t *rule = lv_obj_create(s_bar);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(rule, g_pal.accent, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_40, 0);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *seg = lv_obj_create(s_bar);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, 70, 2);
    lv_obj_set_style_bg_color(seg, g_pal.accent, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(seg, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *seg2 = lv_obj_create(s_bar);
    lv_obj_remove_style_all(seg2);
    lv_obj_set_size(seg2, 18, 2);
    lv_obj_set_style_bg_color(seg2, g_pal.accent2, 0);
    lv_obj_set_style_bg_opa(seg2, LV_OPA_COVER, 0);
    lv_obj_add_flag(seg2, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(seg2, LV_ALIGN_BOTTOM_LEFT, 74, 0);

    s_left = deck_box(s_bar);
    lv_obj_set_height(s_left, LV_PCT(100));
    lv_obj_set_flex_flow(s_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_left, 4, 0);
    lv_obj_align(s_left, LV_ALIGN_LEFT_MID, 6, 0);

    s_clock = deck_label(s_bar, g_font.mono_m, g_pal.text, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, 0);
    /* Tap the clock to sleep the screen. */
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_clock, 8);
    lv_obj_add_event_cb(s_clock, clock_clicked, LV_EVENT_CLICKED, NULL);

    s_right = deck_box(s_bar);
    lv_obj_set_flex_flow(s_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_right, 5, 0);
    lv_obj_align(s_right, LV_ALIGN_RIGHT_MID, -6, 0);
    s_upd = indicator(s_right); /* shown only while an update is waiting */
    lv_obj_add_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
    s_link = indicator(s_right);
    lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
    s_kbd = indicator(s_right);
    s_sd  = indicator(s_right);
    s_net = indicator(s_right);
    s_pwr = deck_icon_text(s_right, "", "", g_font.mono_s, g_pal.dim);

    build_left(NULL, NULL);
    refresh(NULL);
    s_timer = lv_timer_create(refresh, 1000, NULL);
}

void deck_statusbar_set_path(deck_group_t *group, deck_app_t *app)
{
    if (s_left) build_left(group, app);
}

void deck_statusbar_destroy(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_bar) {
        lv_obj_delete(s_bar);
        s_bar = s_left = NULL;
    }
}
