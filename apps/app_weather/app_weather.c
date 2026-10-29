/*
 * WEATHER module: Open-Meteo (no API key) for a location picked by city
 * name. Three views fitted to 320x218: NOW (conditions and readings), 24H
 * (temperature and rain trace) and 7 DAY (outlook strip). The last good
 * response is cached on storage so the screen still fills in offline,
 * marked with its age. The home tile shows the current conditions too.
 *
 * Keys: LEFT / RIGHT or TAB switch views, 1 2 3 jump to one, R refresh,
 * L location, U switch units.
 */
#include "app_weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "weather_data.h"

#define REFRESH_S (15 * 60)
#define HID_L 0x0F
#define HID_R 0x15
#define HID_U 0x18
#define HOUR_LABELS 6 /* one every 4 hours */

enum { VIEW_NOW, VIEW_24H, VIEW_WEEK, VIEW_COUNT };
static const char *const s_view_names[VIEW_COUNT] = {"NOW", "24H", "7 DAY"};

/* Session state: survives leaving and re-entering the module. */
static struct {
    wx_t wx;
    bool have;
    bool from_cache;
    time_t fetched;      /* wall clock of the data (cache file time if cached) */
    bool loading;
    bool failed;
    int view;
} s_w;

/* UI handles, valid while the module is open. */
static struct {
    volatile bool alive; /* cleared on stop so late fetches are dropped */
    lv_obj_t *place;
    lv_obj_t *status;
    lv_obj_t *views[VIEW_COUNT];
    lv_obj_t *tabs[VIEW_COUNT];
    lv_obj_t *units_btn;
    /* NOW */
    lv_obj_t *icon, *temp, *cond, *feels, *range;
    lv_obj_t *kv[5];
    /* 24H */
    lv_obj_t *chart;
    lv_chart_series_t *s_temp, *s_pop;
    lv_obj_t *trace_range;
    lv_obj_t *hour_lbl[HOUR_LABELS];
    /* 7 DAY */
    lv_obj_t *day_name[WX_DAYS], *day_icon[WX_DAYS], *day_hi[WX_DAYS], *day_lo[WX_DAYS], *day_pop[WX_DAYS];
    lv_timer_t *timer;
    uint32_t hold_until; /* a search message stays up until this tick */
    wx_place_t places[WX_PLACES];
    int nplaces;
} s_ui;

enum { KV_HUMID, KV_WIND, KV_PRESS, KV_SUNRISE, KV_SUNSET };

static void render(void);
static void refresh_now(void);
static void tile_render(void);

/* ---- Settings and cache -------------------------------------------------- */

static bool place(char *name, size_t n, double *lat, double *lon)
{
    char a[24], b[24];
    if (!hal_cfg_get_str("wx_name", name, n) || !hal_cfg_get_str("wx_lat", a, sizeof(a)) ||
        !hal_cfg_get_str("wx_lon", b, sizeof(b))) {
        return false;
    }
    *lat = atof(a);
    *lon = atof(b);
    return true;
}

static bool metric(void) { return hal_cfg_get_i32("wx_metric", 0) != 0; }

static bool cache_path(char *out, size_t n)
{
    const char *root = hal_storage_root();
    if (root == NULL || root[0] == '\0') return false;
    snprintf(out, n, "%s/weather.json", root);
    return true;
}

static void cache_save(const char *json, size_t len)
{
    char path[96];
    if (!cache_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite(json, 1, len, f);
    fclose(f);
}

static void cache_load(void)
{
    char path[96];
    if (!cache_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n > 0 && n < 64 * 1024) ? malloc((size_t)n + 1) : NULL;
    if (buf) {
        buf[fread(buf, 1, (size_t)n, f)] = '\0';
        if (wx_parse(buf, &s_w.wx)) {
            s_w.have       = true;
            s_w.from_cache = true;
            struct stat st;
            s_w.fetched = (stat(path, &st) == 0) ? st.st_mtime : 0;
        }
        free(buf);
    }
    fclose(f);
}

/* ---- Fetching ------------------------------------------------------------ */

static void on_forecast(char *body, size_t len, void *user)
{
    (void)user;
    s_w.loading = false;
    static wx_t fresh;
    if (body && wx_parse(body, &fresh)) {
        s_w.wx         = fresh;
        s_w.have       = true;
        s_w.from_cache = false;
        s_w.failed     = false;
        s_w.fetched    = time(NULL);
        cache_save(body, len);
    } else {
        s_w.failed = true;
    }
    render();
    tile_render();
}

/* `alive` is whoever asked (the module or the home tile): the reply is
 * dropped once that screen is gone. */
static void refresh_with(volatile bool *alive)
{
    char name[48];
    double lat, lon;
    if (s_w.loading || !place(name, sizeof(name), &lat, &lon)) return;
    if (net_state() != NET_CONNECTED) {
        render();
        tile_render();
        return;
    }
    char url[512];
    wx_forecast_url(url, sizeof(url), lat, lon, metric());
    s_w.loading = true;
    render();
    tile_render();
    net_fetch(url, on_forecast, NULL, alive);
}

static void refresh_now(void) { refresh_with(&s_ui.alive); }

static bool due(void)
{
    return net_state() == NET_CONNECTED && !s_w.loading &&
           (!s_w.have || s_w.from_cache || time(NULL) - s_w.fetched > REFRESH_S);
}

/* ---- Location ------------------------------------------------------------ */

static void set_status(const char *text, lv_color_t c)
{
    if (!s_ui.alive) return;
    lv_label_set_text(s_ui.status, text);
    lv_obj_set_style_text_color(s_ui.status, c, 0);
}

/* A message about the city search, kept up for a few seconds instead of
 * being replaced by the freshness readout on the next tick. */
static void flash_status(const char *text, lv_color_t c)
{
    set_status(text, c);
    s_ui.hold_until = lv_tick_get() + 4000;
}

static void place_picked(int index, void *user)
{
    (void)user;
    if (index < 0 || index >= s_ui.nplaces) return;
    const wx_place_t *p = &s_ui.places[index];
    char a[24], b[24];
    snprintf(a, sizeof(a), "%.4f", p->lat);
    snprintf(b, sizeof(b), "%.4f", p->lon);
    hal_cfg_set_str("wx_name", p->name);
    hal_cfg_set_str("wx_lat", a);
    hal_cfg_set_str("wx_lon", b);
    s_w.have = false; /* old city's data no longer applies, cached or not */
    char path[96];
    if (cache_path(path, sizeof(path))) remove(path);
    refresh_now();
}

static void on_places(char *body, size_t len, void *user)
{
    (void)len;
    (void)user;
    s_ui.hold_until = 0;
    s_ui.nplaces    = body ? wx_parse_places(body, s_ui.places, WX_PLACES) : 0;
    if (s_ui.nplaces == 0) {
        flash_status(body ? "NO MATCH" : "LOOKUP FAILED", g_pal.danger);
        return;
    }
    const char *items[WX_PLACES];
    for (int i = 0; i < s_ui.nplaces; i++) items[i] = s_ui.places[i].label;
    deck_modal_list("LOCATION", items, s_ui.nplaces, place_picked, NULL);
}

static void city_entered(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == '\0') return;
    if (net_state() != NET_CONNECTED) {
        flash_status("JOIN WI-FI IN SYSTEM", g_pal.warn);
        return;
    }
    char url[512];
    wx_geocode_url(url, sizeof(url), text);
    flash_status("SEARCHING...", g_pal.accent);
    net_fetch(url, on_places, NULL, &s_ui.alive);
}

static void ask_location(void) { deck_modal_prompt("CITY", "", city_entered, NULL); }

/* ---- Rendering ----------------------------------------------------------- */

static void fmt_temp(char *buf, size_t n, float t) { snprintf(buf, n, "%.0f\xC2\xB0", (double)t); }

static bool stale(void) { return s_w.from_cache || s_w.failed || net_state() != NET_CONNECTED; }

static void render_status(void)
{
    if (s_ui.hold_until && (int32_t)(s_ui.hold_until - lv_tick_get()) > 0) return;
    s_ui.hold_until = 0;
    char buf[40];
    lv_color_t c = g_pal.dim;
    char name[48];
    double lat, lon;
    if (!place(name, sizeof(name), &lat, &lon)) {
        snprintf(buf, sizeof(buf), "PRESS L");
        c = g_pal.warn;
    } else if (s_w.loading) {
        snprintf(buf, sizeof(buf), "UPLINK...");
        c = g_pal.accent;
    } else if (s_w.have) {
        long age = s_w.fetched ? (long)(time(NULL) - s_w.fetched) : -1;
        const char *tag = stale() ? "CACHED" : "LIVE";
        if (age >= 0 && age < 90) {
            snprintf(buf, sizeof(buf), "%s NOW", tag);
        } else if (age >= 0 && age < 7200) {
            snprintf(buf, sizeof(buf), "%s %ldM", tag, age / 60);
        } else if (age >= 0) {
            snprintf(buf, sizeof(buf), "%s %ldH", tag, age / 3600);
        } else {
            snprintf(buf, sizeof(buf), "%s", tag);
        }
        c = stale() ? g_pal.warn : g_pal.ok;
    } else if (net_state() != NET_CONNECTED) {
        snprintf(buf, sizeof(buf), "OFFLINE");
        c = g_pal.warn;
    } else {
        snprintf(buf, sizeof(buf), s_w.failed ? "FAILED, PRESS R" : "NO DATA");
        c = s_w.failed ? g_pal.danger : g_pal.dim;
    }
    set_status(buf, c);
}

static void show_view(int v)
{
    s_w.view = (v + VIEW_COUNT) % VIEW_COUNT;
    if (!s_ui.alive) return;
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (i == s_w.view) {
            lv_obj_remove_flag(s_ui.views[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(s_ui.tabs[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(s_ui.views[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(s_ui.tabs[i], LV_STATE_CHECKED);
        }
        lv_obj_send_event(s_ui.tabs[i], LV_EVENT_STATE_CHANGED, NULL);
    }
}

static void render_empty(void)
{
    lv_label_set_text(s_ui.temp, "--");
    lv_label_set_text(s_ui.icon, WX_CLOUDY);
    lv_label_set_text(s_ui.cond, "");
    lv_label_set_text(s_ui.feels, "");
    lv_label_set_text(s_ui.range, "");
    for (int i = 0; i < 5; i++) lv_label_set_text(s_ui.kv[i], "--");
    lv_chart_set_all_values(s_ui.chart, s_ui.s_temp, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(s_ui.chart, s_ui.s_pop, LV_CHART_POINT_NONE);
    lv_label_set_text(s_ui.trace_range, "");
    for (int i = 0; i < HOUR_LABELS; i++) lv_label_set_text(s_ui.hour_lbl[i], "");
    for (int i = 0; i < WX_DAYS; i++) {
        lv_label_set_text(s_ui.day_name[i], "---");
        lv_label_set_text(s_ui.day_icon[i], "");
        lv_label_set_text(s_ui.day_hi[i], "");
        lv_label_set_text(s_ui.day_lo[i], "");
        lv_label_set_text(s_ui.day_pop[i], "");
    }
}

static void render(void)
{
    if (!s_ui.alive) return;
    char name[48] = "";
    double lat, lon;
    place(name, sizeof(name), &lat, &lon);
    deck_icon_text_set(s_ui.place, ICON_PIN, name[0] ? name : "NO LOCATION", name[0] ? g_pal.text : g_pal.warn);
    lv_label_set_text(lv_obj_get_child(s_ui.units_btn, 0), metric() ? "\xC2\xB0" "C" : "\xC2\xB0" "F");
    render_status();
    if (!s_w.have || name[0] == '\0') {
        render_empty();
        return;
    }

    const wx_t *w = &s_w.wx;
    char buf[48], hi[12], lo[12];

    /* NOW */
    fmt_temp(buf, sizeof(buf), w->temp);
    lv_label_set_text(s_ui.temp, buf);
    lv_label_set_text(s_ui.icon, wx_code_icon(w->code, w->is_day));
    lv_label_set_text(s_ui.cond, wx_code_text(w->code));
    fmt_temp(buf, sizeof(buf), w->feels);
    lv_label_set_text_fmt(s_ui.feels, "FEELS %s", buf);
    fmt_temp(hi, sizeof(hi), w->day[0].hi);
    fmt_temp(lo, sizeof(lo), w->day[0].lo);
    lv_label_set_text_fmt(s_ui.range, "H %s  L %s", hi, lo);
    lv_label_set_text_fmt(s_ui.kv[KV_HUMID], "%d%%", w->humidity);
    lv_label_set_text_fmt(s_ui.kv[KV_WIND], "%.0f %s %s", (double)w->wind, w->wind_unit, wx_compass(w->wind_dir));
    lv_label_set_text_fmt(s_ui.kv[KV_PRESS], "%.0f hPa", (double)w->pressure);
    lv_label_set_text(s_ui.kv[KV_SUNRISE], w->sunrise[0] ? w->sunrise : "--");
    lv_label_set_text(s_ui.kv[KV_SUNSET], w->sunset[0] ? w->sunset : "--");

    /* 24H: temperature on the left axis, rain chance 0..100 on the right. */
    float tmin = 1e9f, tmax = -1e9f;
    int pmax = 0;
    for (int i = 0; i < w->hours; i++) {
        tmin = fminf(tmin, w->hour_temp[i]);
        tmax = fmaxf(tmax, w->hour_temp[i]);
        if (w->hour_pop[i] > pmax) pmax = w->hour_pop[i];
    }
    if (w->hours > 0) {
        lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(tmin) - 2,
                                (int32_t)ceilf(tmax) + 2);
        fmt_temp(lo, sizeof(lo), tmin);
        fmt_temp(hi, sizeof(hi), tmax);
        lv_label_set_text_fmt(s_ui.trace_range, "%s..%s  RAIN %d%%", lo, hi, pmax);
    } else {
        lv_label_set_text(s_ui.trace_range, "");
    }
    for (int i = 0; i < WX_HOURS; i++) {
        bool ok = i < w->hours;
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.s_temp, (uint32_t)i,
                                        ok ? (int32_t)lroundf(w->hour_temp[i]) : LV_CHART_POINT_NONE);
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.s_pop, (uint32_t)i, ok ? w->hour_pop[i] : LV_CHART_POINT_NONE);
    }
    lv_chart_refresh(s_ui.chart);
    for (int i = 0; i < HOUR_LABELS; i++) {
        int idx = i * (WX_HOURS / HOUR_LABELS);
        if (idx < w->hours) {
            lv_label_set_text_fmt(s_ui.hour_lbl[i], "%02d", w->hour_of_day[idx]);
        } else {
            lv_label_set_text(s_ui.hour_lbl[i], "");
        }
    }

    /* 7 DAY */
    for (int i = 0; i < WX_DAYS; i++) {
        if (i >= w->days) continue;
        const wx_day_t *d = &w->day[i];
        lv_label_set_text(s_ui.day_name[i], i == 0 ? "TODAY" : d->weekday);
        lv_label_set_text(s_ui.day_icon[i], wx_code_icon(d->code, true));
        fmt_temp(buf, sizeof(buf), d->hi);
        lv_label_set_text(s_ui.day_hi[i], buf);
        fmt_temp(buf, sizeof(buf), d->lo);
        lv_label_set_text(s_ui.day_lo[i], buf);
        lv_label_set_text_fmt(s_ui.day_pop[i], "%d%%", d->pop);
        lv_obj_set_style_text_color(s_ui.day_pop[i], d->pop >= 50 ? g_pal.accent2 : g_pal.dim, 0);
    }
}

/* ---- Building ------------------------------------------------------------ */

static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, g_font.mono_s);
    lv_obj_set_style_min_height(b, 20, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_set_style_pad_hor(b, 10, 0); /* clears the 10 px diagonal cut */
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *kv_row(lv_obj_t *parent, const char *icon, const char *key)
{
    lv_obj_t *r = deck_box(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_t *k = deck_icon_text(r, icon, key, g_font.mono_s, g_pal.dim);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = deck_label(r, g_font.mono_s, g_pal.text, "--");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static void tab_clicked(lv_event_t *e) { show_view((int)(intptr_t)lv_event_get_user_data(e)); }

static void loc_clicked(lv_event_t *e)
{
    (void)e;
    ask_location();
}

static void units_clicked(lv_event_t *e)
{
    (void)e;
    hal_cfg_set_i32("wx_metric", !metric());
    refresh_now();
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    refresh_now();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (due()) {
        refresh_now();
    } else {
        render_status();
    }
}

static lv_obj_t *view_box(lv_obj_t *area)
{
    lv_obj_t *v = deck_box(area);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    return v;
}

static void build_now(lv_obj_t *v)
{
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(v, 8, 0);

    lv_obj_t *hero = deck_panel(v, DECK_CUT_TL | DECK_CUT_BR, 8);
    lv_obj_set_size(hero, 150, LV_PCT(100));
    deck_panel_set_tab(hero, true);
    lv_obj_set_style_pad_all(hero, 8, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(hero, 4, 0);

    lv_obj_t *now = deck_box(hero);
    lv_obj_set_size(now, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 6, 0);
    s_ui.icon = deck_label(now, g_font.icon_l, g_pal.accent, "");
    s_ui.temp = deck_label(now, g_font.disp_hero, g_pal.text, "--");

    s_ui.cond = deck_label(hero, g_font.disp_s, g_pal.accent2, "");
    lv_label_set_long_mode(s_ui.cond, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_ui.cond, LV_PCT(100));
    s_ui.feels = deck_label(hero, g_font.mono_s, g_pal.dim, "");
    s_ui.range = deck_label(hero, g_font.mono_s, g_pal.text, "");

    lv_obj_t *kv = deck_box(v);
    lv_obj_set_height(kv, LV_PCT(100));
    lv_obj_set_flex_grow(kv, 1);
    lv_obj_set_flex_flow(kv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(kv, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_ui.kv[KV_HUMID]   = kv_row(kv, ICON_HUMIDITY, "HUMID");
    s_ui.kv[KV_WIND]    = kv_row(kv, WX_WIND, "WIND");
    s_ui.kv[KV_PRESS]   = kv_row(kv, ICON_GAUGE, "PRESS");
    s_ui.kv[KV_SUNRISE] = kv_row(kv, WX_SUNRISE, "RISE");
    s_ui.kv[KV_SUNSET]  = kv_row(kv, WX_SUNSET, "SET");
}

static void build_24h(lv_obj_t *v)
{
    lv_obj_t *trace = deck_panel(v, DECK_CUT_TL | DECK_CUT_BR, 8);
    lv_obj_set_size(trace, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(trace, 6, 0);
    lv_obj_set_flex_flow(trace, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(trace, 2, 0);

    lv_obj_t *legend = deck_box(trace);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 8, 0);
    deck_label(legend, g_font.mono_s, g_pal.accent, "TEMP");
    deck_label(legend, g_font.mono_s, g_pal.accent2, "RAIN %");
    lv_obj_t *gap = deck_box(legend);
    lv_obj_set_size(gap, 1, 1);
    lv_obj_set_flex_grow(gap, 1);
    s_ui.trace_range = deck_label(legend, g_font.mono_s, g_pal.dim, "");

    s_ui.chart = lv_chart_create(trace);
    lv_obj_set_width(s_ui.chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.chart, 1);
    lv_chart_set_type(s_ui.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_ui.chart, WX_HOURS);
    lv_chart_set_div_line_count(s_ui.chart, 3, 5);
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(s_ui.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.chart, 0, 0);
    lv_obj_set_style_pad_all(s_ui.chart, 2, 0);
    lv_obj_set_style_line_color(s_ui.chart, g_pal.grid, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ui.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_ui.chart, 0, 0, LV_PART_INDICATOR);
    s_ui.s_pop  = lv_chart_add_series(s_ui.chart, g_pal.accent2, LV_CHART_AXIS_SECONDARY_Y);
    s_ui.s_temp = lv_chart_add_series(s_ui.chart, g_pal.accent, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *hours = deck_box(trace);
    lv_obj_set_size(hours, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hours, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hours, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < HOUR_LABELS; i++) s_ui.hour_lbl[i] = deck_label(hours, g_font.mono_s, g_pal.dim, "");
}

static void build_week(lv_obj_t *v)
{
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(v, 3, 0);
    for (int i = 0; i < WX_DAYS; i++) {
        lv_obj_t *d = deck_panel(v, DECK_CUT_BR, 6);
        lv_obj_set_height(d, LV_PCT(100));
        lv_obj_set_flex_grow(d, 1);
        lv_obj_set_style_pad_ver(d, 6, 0);
        lv_obj_set_style_pad_hor(d, 0, 0);
        lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(d, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (i == 0) deck_panel_set_tab(d, true);
        s_ui.day_name[i] = deck_label(d, g_font.mono_s, i == 0 ? g_pal.accent : g_pal.text, "");
        s_ui.day_icon[i] = deck_label(d, g_font.icon_m, g_pal.accent, "");
        s_ui.day_hi[i]   = deck_label(d, g_font.mono_m, g_pal.text, "");
        s_ui.day_lo[i]   = deck_label(d, g_font.mono_s, g_pal.dim, "");
        s_ui.day_pop[i]  = deck_label(d, g_font.mono_s, g_pal.dim, "");
    }
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.alive = true;
    if (!s_w.have) cache_load();

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(root, 6, 0);
    lv_obj_set_style_pad_ver(root, 4, 0);
    lv_obj_set_style_pad_row(root, 4, 0);

    /* Header: where, and how fresh. */
    lv_obj_t *head = deck_box(root);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    s_ui.place = deck_icon_text(head, ICON_PIN, "", g_font.mono_s, g_pal.text);
    lv_obj_align(s_ui.place, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.status = deck_label(head, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_ui.status, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Views, one shown at a time. */
    lv_obj_t *area = deck_box(root);
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_set_flex_grow(area, 1);
    for (int i = 0; i < VIEW_COUNT; i++) s_ui.views[i] = view_box(area);
    build_now(s_ui.views[VIEW_NOW]);
    build_24h(s_ui.views[VIEW_24H]);
    build_week(s_ui.views[VIEW_WEEK]);

    /* Toolbar: view tabs, then the actions. */
    lv_obj_t *bar = deck_box(root);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 3, 0);
    for (int i = 0; i < VIEW_COUNT; i++) s_ui.tabs[i] = tool_button(bar, s_view_names[i], tab_clicked, (void *)(intptr_t)i);
    lv_obj_t *gap = deck_box(bar);
    lv_obj_set_size(gap, 1, 1);
    lv_obj_set_flex_grow(gap, 1);
    tool_button(bar, "CITY", loc_clicked, NULL);
    s_ui.units_btn = tool_button(bar, "\xC2\xB0" "F", units_clicked, NULL);
    tool_button(bar, "REFRESH", refresh_clicked, NULL);

    show_view(s_w.view);
    render();
    s_ui.timer = lv_timer_create(tick, 1000, NULL);
    tick(s_ui.timer);

    char name[48];
    double lat, lon;
    if (!place(name, sizeof(name), &lat, &lon)) ask_location();
    return true;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    switch (k->code) {
        case HID_R: refresh_now(); return true;
        case HID_L: ask_location(); return true;
        case HID_U: units_clicked(NULL); return true;
        case HID_RIGHT:
        case HID_TAB: show_view(s_w.view + 1); return true;
        case HID_LEFT: show_view(s_w.view - 1); return true;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + VIEW_COUNT) {
                show_view(k->code - HID_1);
                return true;
            }
            return false;
    }
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    memset(&s_ui, 0, sizeof(s_ui)); /* alive = false: in-flight fetches are dropped */
    s_w.loading = false;
}

/* ---- Home tile -----------------------------------------------------------
 * On this deck's 60 px tiles the widget is the tile itself: its icon turns
 * into the current conditions and its name line into the temperature. Same
 * cache and fetch path as the module; lives as long as the home screen. */

static struct {
    volatile bool alive;
    lv_obj_t *icon, *label;
    lv_timer_t *timer;
} s_tile;

static void tile_render(void)
{
    if (!s_tile.alive) return;
    char name[48];
    double lat, lon;
    if (!s_w.have || !place(name, sizeof(name), &lat, &lon)) {
        lv_label_set_text(s_tile.icon, ICON_WEATHER);
        lv_label_set_text(s_tile.label, "WEATHER");
        lv_obj_set_style_text_color(s_tile.label, g_pal.text, 0);
        return;
    }
    char buf[16];
    fmt_temp(buf, sizeof(buf), s_w.wx.temp);
    lv_label_set_text(s_tile.icon, wx_code_icon(s_w.wx.code, s_w.wx.is_day));
    lv_label_set_text(s_tile.label, buf);
    lv_obj_set_style_text_color(s_tile.label, stale() ? g_pal.warn : g_pal.text, 0);
}

static void tile_tick(lv_timer_t *t)
{
    (void)t;
    if (due()) refresh_with(&s_tile.alive);
    tile_render();
}

static void tile_deleted(lv_event_t *e)
{
    (void)e;
    if (s_tile.timer) lv_timer_delete(s_tile.timer);
    memset(&s_tile, 0, sizeof(s_tile)); /* alive = false: an in-flight fetch is dropped */
    s_w.loading = false;
}

static void on_tile(deck_app_t *self, lv_obj_t *body, lv_obj_t *icon)
{
    (void)self;
    memset(&s_tile, 0, sizeof(s_tile));
    s_tile.alive = true;
    s_tile.icon  = icon;
    s_tile.label = body;
    if (!s_w.have) cache_load();
    lv_obj_add_event_cb(body, tile_deleted, LV_EVENT_DELETE, NULL);
    s_tile.timer = lv_timer_create(tile_tick, 5000, NULL);
    tile_tick(s_tile.timer);
}

static deck_app_t s_app = {
    .name     = "WEATHER",
    .tagline  = "OPEN-METEO",
    .icon     = ICON_WEATHER,
    .eta      = NULL,
    .hotkey   = HID_LETTER('W'),
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
    .on_tile  = on_tile,
};

deck_app_t *app_weather(void) { return &s_app; }
