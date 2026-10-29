/*
 * NET TOOLS: the CoreS3's own radio as a toolkit, fitted to 320x218. Four
 * tabs: SCAN (every access point in earshot with a channel chart), PING
 * (ping, DNS, traceroute, port check), LAN (who is on the subnet) and SPEED
 * (down and up). The blocking work runs on the net_tools worker.
 *
 * Keys: 1..4 tabs (outside the host field), R rescan / rerun, UP / DOWN
 * select, ENTER details or run, TAB next field.
 */
#include "app_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "net_tools.h"

#define TABS 4
#define TAB_SCAN 0
#define TAB_PING 1
#define TAB_LAN 2
#define TAB_SPEED 3

#define BSS_MAX 48
#define HOSTS_MAX 64
#define CHANNELS 13
#define RESCAN_MS 30000 /* a rebuild loses nothing, but scanning costs airtime */

#define HID_R 0x15

static const char *const s_tab_names[TABS] = {"SCAN", "PING", "LAN", "SPEED"};

static struct {
    int tab;
    lv_obj_t *tab_btn[TABS];
    lv_obj_t *page[TABS];
    lv_obj_t *status;
    lv_timer_t *timer;

    /* SCAN */
    net_bss_t bss[BSS_MAX];
    int count;
    lv_obj_t *list;
    lv_obj_t *chart;
    lv_chart_series_t *strength;
    lv_obj_t *ch_count[CHANNELS];
    bool scanning;
    uint32_t scanned_at; /* lv tick of the last finished scan, 0 = none */
    char detail[320];

    /* PING */
    lv_obj_t *host_ta;
    lv_obj_t *out;
    lv_obj_t *out_box;
    lv_obj_t *ping_btn[4];
    char host[96];
    int job;             /* which tool is running, -1 none */
    int port;
    char result[1536];
    net_ping_reply_t pings[5];
    int ping_got;
    net_hop_t hops[20];
    int hop_n;
    float port_ms;
    char ip[16];
    bool ok;

    /* LAN */
    net_host_t hosts[HOSTS_MAX];
    int host_n;
    lv_obj_t *lan_list;
    lv_obj_t *lan_status;
    lv_obj_t *lan_btn;
    volatile int lan_done, lan_total; /* progress from the worker */
    bool lan_running;

    /* SPEED */
    lv_obj_t *spd_btn;
    lv_obj_t *spd_status;
    lv_obj_t *spd_down, *spd_up, *spd_lat;
    volatile int spd_phase; /* 0 idle, 1 latency, 2 down, 3 up, 4 done */
    bool spd_lat_ok, spd_down_ok, spd_up_ok;
    float spd_lat_ms, spd_down_ms, spd_up_ms;
    size_t spd_down_bytes;
} s_ui DECK_PSRAM_BSS;

#define SPEED_HOST "speed.cloudflare.com"
/* Plain HTTP for the download so the number is the link, not TLS decryption. */
#define SPEED_DOWN_URL "http://ipv4.download.thinkbroadband.com/10MB.zip"
#define SPEED_UP_URL "https://speed.cloudflare.com/__up"
#define SPEED_DOWN_BYTES (8u * 1000u * 1000u)
#define SPEED_UP_BYTES (2u * 1000u * 1000u)

enum { JOB_PING, JOB_DNS, JOB_TRACE, JOB_PORT };

/* ---- Helpers ------------------------------------------------------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = deck_button(parent, text, g_font.mono_s);
    lv_obj_set_style_min_height(b, 22, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 3, 0);
    lv_obj_set_style_pad_hor(b, 10, 0); /* clears the 10 px diagonal cut */
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static lv_obj_t *scroll_list(lv_obj_t *parent)
{
    lv_obj_t *l = deck_box(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 3, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
    return l;
}

static void set_status(const char *text) { lv_label_set_text(s_ui.status, text); }

static int by_rssi(const void *a, const void *b)
{
    const net_bss_t *x = a, *y = b;
    return y->rssi - x->rssi;
}

static const char *signal_word(int rssi)
{
    return rssi >= -55 ? "EXCELLENT" : rssi >= -65 ? "GOOD" : rssi >= -75 ? "FAIR" : "WEAK";
}

static lv_color_t signal_color(int rssi)
{
    return rssi >= -65 ? g_pal.ok : rssi >= -75 ? g_pal.warn : g_pal.danger;
}

static int channel_mhz(int ch) { return ch == 14 ? 2484 : 2407 + ch * 5; }

static void lan_tick(void);
static void speed_tick(void);

/* ---- SCAN ---------------------------------------------------------------- */

static void row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ui.count) return;
    const net_bss_t *b = &s_ui.bss[i];
    snprintf(s_ui.detail, sizeof(s_ui.detail),
             "SSID     %s\n"
             "BSSID    %02X:%02X:%02X:%02X:%02X:%02X\n"
             "CHANNEL  %d (%d MHz)\n"
             "SIGNAL   %d dBm %s\n"
             "SECURITY %s",
             b->ssid[0] ? b->ssid : "(hidden)", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4],
             b->bssid[5], b->channel, channel_mhz(b->channel), b->rssi, signal_word(b->rssi), b->auth);
    const char *cols[] = {s_ui.detail};
    deck_modal_info("NETWORK", cols, 1);
}

static void render_scan(void)
{
    /* Rebuilding the rows must not throw away where the user was. */
    int32_t scroll_y  = lv_obj_get_scroll_y(s_ui.list);
    lv_obj_t *focused = lv_group_get_focused(deck_input_group());
    int focus_idx     = -1;
    if (focused && lv_obj_get_parent(focused) == s_ui.list) focus_idx = (int)lv_obj_get_index(focused);
    lv_obj_clean(s_ui.list);
    if (s_ui.tab == TAB_SCAN) lv_group_remove_all_objs(deck_input_group());
    if (s_ui.count == 0) {
        deck_label(s_ui.list, g_font.mono_s, g_pal.dim,
                   net_state() == NET_NO_RADIO ? "No radio." : s_ui.scanning ? "Scanning..." : "Nothing heard yet.");
    }
    for (int i = 0; i < s_ui.count; i++) {
        const net_bss_t *b = &s_ui.bss[i];
        lv_obj_t *row      = deck_panel(s_ui.list, DECK_CUT_BR, 5);
        lv_obj_set_size(row, LV_PCT(100), 20);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 5, 0);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (s_ui.tab == TAB_SCAN) lv_group_add_obj(deck_input_group(), row);

        lv_obj_t *name = deck_label(row, g_font.mono_s, b->ssid[0] ? g_pal.text : g_pal.dim,
                                    b->ssid[0] ? b->ssid : "(hidden)");
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, 150);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char ch[8];
        snprintf(ch, sizeof(ch), "%d", b->channel);
        lv_obj_t *c = deck_label(row, g_font.mono_s, g_pal.accent2, ch);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, 158, 0);

        lv_obj_t *auth = deck_label(row, g_font.mono_s, strcmp(b->auth, "OPEN") == 0 ? g_pal.warn : g_pal.dim, b->auth);
        lv_obj_align(auth, LV_ALIGN_LEFT_MID, 180, 0);

        char sig[8];
        snprintf(sig, sizeof(sig), "%d", b->rssi);
        lv_obj_t *s = deck_label(row, g_font.mono_s, signal_color(b->rssi), sig);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_update_layout(s_ui.list);
    lv_obj_scroll_to_y(s_ui.list, scroll_y, LV_ANIM_OFF);
    if (focus_idx >= 0 && focus_idx < s_ui.count && s_ui.tab == TAB_SCAN) {
        lv_obj_t *row = lv_obj_get_child(s_ui.list, focus_idx);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /* keep the scroll where it is */
        lv_group_focus_obj(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }

    /* Channel chart: the strongest signal on each channel, with a count. */
    int best[CHANNELS + 1], count[CHANNELS + 1];
    for (int c = 0; c <= CHANNELS; c++) {
        best[c]  = -100;
        count[c] = 0;
    }
    for (int i = 0; i < s_ui.count; i++) {
        int c = s_ui.bss[i].channel;
        if (c < 1 || c > CHANNELS) continue;
        count[c]++;
        if (s_ui.bss[i].rssi > best[c]) best[c] = s_ui.bss[i].rssi;
    }
    for (int c = 1; c <= CHANNELS; c++) {
        int v = count[c] ? (best[c] + 100) * 100 / 70 : 0; /* -100..-30 dBm to 0..100 */
        if (v > 100) v = 100;
        if (count[c] && v < 8) v = 8; /* a heard network always shows a stub */
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.strength, (uint32_t)(c - 1), count[c] ? v : LV_CHART_POINT_NONE);
        if (count[c]) {
            lv_label_set_text_fmt(s_ui.ch_count[c - 1], "%d", count[c]);
            lv_obj_set_style_text_color(s_ui.ch_count[c - 1], g_pal.accent2, 0);
        } else {
            lv_label_set_text_fmt(s_ui.ch_count[c - 1], "%d", c);
            lv_obj_set_style_text_color(s_ui.ch_count[c - 1], g_pal.dim, 0);
        }
    }
    lv_chart_refresh(s_ui.chart);
}

static void render_scan_status(void)
{
    char buf[40];
    if (s_ui.scanning) {
        snprintf(buf, sizeof(buf), "SCANNING...");
    } else if (s_ui.scanned_at == 0) {
        snprintf(buf, sizeof(buf), "R RESCAN");
    } else {
        unsigned age = lv_tick_elaps(s_ui.scanned_at) / 1000;
        snprintf(buf, sizeof(buf), "%d NET%s  %uS", s_ui.count, s_ui.count == 1 ? "" : "S", age);
    }
    set_status(buf);
}

static void start_scan(void)
{
    if (s_ui.scanning) return;
    if (net_scan_start()) s_ui.scanning = true;
    if (s_ui.tab == TAB_SCAN) render_scan_status();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    lan_tick();
    speed_tick();
    if (s_ui.scanning && net_scan_done()) {
        s_ui.scanning = false;
        s_ui.count    = (int)net_scan_bss(s_ui.bss, BSS_MAX);
        qsort(s_ui.bss, (size_t)s_ui.count, sizeof(s_ui.bss[0]), by_rssi);
        s_ui.scanned_at = lv_tick_get();
        render_scan();
    }
    if (s_ui.tab != TAB_SCAN) return;
    if (!s_ui.scanning && (s_ui.scanned_at == 0 || lv_tick_elaps(s_ui.scanned_at) >= RESCAN_MS)) start_scan();
    render_scan_status();
}

static void build_scan(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 3, 0);

    /* Channel chart across the top: bar = strongest signal, number under it
     * = how many networks (the channel number itself when none). */
    lv_obj_t *top = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 6);
    lv_obj_set_size(top, LV_PCT(100), 62);
    lv_obj_set_style_pad_hor(top, 6, 0);
    lv_obj_set_style_pad_ver(top, 3, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(top, 0, 0);

    s_ui.chart = lv_chart_create(top);
    lv_obj_set_width(s_ui.chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.chart, 1);
    lv_chart_set_type(s_ui.chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_ui.chart, CHANNELS);
    lv_chart_set_div_line_count(s_ui.chart, 3, 0);
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(s_ui.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.chart, 0, 0);
    lv_obj_set_style_pad_all(s_ui.chart, 1, 0);
    lv_obj_set_style_pad_column(s_ui.chart, 4, 0);
    lv_obj_set_style_line_color(s_ui.chart, g_pal.grid, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.chart, 0, LV_PART_ITEMS);
    s_ui.strength = lv_chart_add_series(s_ui.chart, g_pal.accent, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *nums = deck_box(top);
    lv_obj_set_size(nums, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nums, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nums, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int c = 1; c <= CHANNELS; c++) {
        char n[4];
        snprintf(n, sizeof(n), "%d", c);
        s_ui.ch_count[c - 1] = deck_label(nums, g_font.mono_s, g_pal.dim, n);
    }

    s_ui.list = scroll_list(page);
}

/* ---- PING / DNS / TRACE / PORT ------------------------------------------- */

static void ping_set_busy(bool busy)
{
    for (int i = 0; i < 4; i++) {
        if (busy) {
            lv_obj_add_state(s_ui.ping_btn[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_ui.ping_btn[i], LV_STATE_DISABLED);
        }
    }
}

/* Worker task: do the blocking work into s_ui, nothing LVGL. */
static void ping_job(void *u)
{
    (void)u;
    switch (s_ui.job) {
        case JOB_PING:
            s_ui.ping_got = net_ping(s_ui.host, 5, 1500, s_ui.pings);
            s_ui.ok       = s_ui.ping_got >= 0;
            break;
        case JOB_DNS: s_ui.ok = net_resolve(s_ui.host, s_ui.ip, sizeof(s_ui.ip)); break;
        case JOB_TRACE:
            s_ui.hop_n = net_trace(s_ui.host, 20, 1200, s_ui.hops);
            s_ui.ok    = s_ui.hop_n >= 0;
            break;
        case JOB_PORT: s_ui.ok = net_port_open(s_ui.host, s_ui.port, 2500, &s_ui.port_ms); break;
        default: break;
    }
}

/* Lines are kept under ~46 characters: the width of the output box. */
static void ping_done(void *u)
{
    (void)u;
    if (s_ui.out == NULL) return; /* the module closed meanwhile */
    char *r  = s_ui.result;
    size_t n = sizeof(s_ui.result);
    size_t o = 0;
    switch (s_ui.job) {
        case JOB_PING: {
            if (!s_ui.ok) {
                o += (size_t)snprintf(r + o, n - o, "PING %s\ncould not resolve or open a socket", s_ui.host);
                break;
            }
            const char *from = "";
            for (int i = 0; i < 5; i++) {
                if (s_ui.pings[i].ok) {
                    from = s_ui.pings[i].from;
                    break;
                }
            }
            o += (size_t)snprintf(r + o, n - o, "PING %s\n%s%s%s", s_ui.host, from[0] ? "from " : "", from,
                                  from[0] ? "\n" : "");
            float min = 1e9f, max = 0, sum = 0;
            for (int i = 0; i < 5 && o < n - 60; i++) {
                const net_ping_reply_t *p = &s_ui.pings[i];
                if (p->ok) {
                    o += (size_t)snprintf(r + o, n - o, " seq %d  ttl %-3d %7.1f ms\n", p->seq, p->ttl, (double)p->ms);
                    if (p->ms < min) min = p->ms;
                    if (p->ms > max) max = p->ms;
                    sum += p->ms;
                } else {
                    o += (size_t)snprintf(r + o, n - o, " seq %d  timeout\n", p->seq);
                }
            }
            if (s_ui.ping_got) {
                o += (size_t)snprintf(r + o, n - o, "%d/5 ok  min %.1f avg %.1f max %.1f", s_ui.ping_got, (double)min,
                                      (double)(sum / (float)s_ui.ping_got), (double)max);
            } else {
                o += (size_t)snprintf(r + o, n - o, "no answer: host down or ICMP blocked");
            }
            break;
        }
        case JOB_DNS:
            o += (size_t)snprintf(r + o, n - o, "DNS %s\n%s", s_ui.host, s_ui.ok ? s_ui.ip : "no such name");
            break;
        case JOB_TRACE:
            if (!s_ui.ok) {
                o += (size_t)snprintf(r + o, n - o, "TRACE %s\ncould not resolve or open a socket", s_ui.host);
                break;
            }
            o += (size_t)snprintf(r + o, n - o, "TRACE %s\n", s_ui.host);
            for (int i = 0; i < s_ui.hop_n && o < n - 50; i++) {
                const net_hop_t *h = &s_ui.hops[i];
                if (h->ip[0]) {
                    o += (size_t)snprintf(r + o, n - o, "%2d %-15s %6.1f ms%s\n", h->hop, h->ip, (double)h->ms,
                                          h->reached ? " *" : "");
                } else {
                    o += (size_t)snprintf(r + o, n - o, "%2d -\n", h->hop);
                }
            }
            if (s_ui.hop_n && !s_ui.hops[s_ui.hop_n - 1].reached) {
                o += (size_t)snprintf(r + o, n - o, "stopped without reaching it");
            } else if (s_ui.hop_n) {
                o += (size_t)snprintf(r + o, n - o, "* = destination");
            }
            break;
        case JOB_PORT:
            o += (size_t)snprintf(r + o, n - o, "PORT %s:%d\n%s (%.0f ms)", s_ui.host, s_ui.port,
                                  s_ui.ok ? "OPEN: something listens" : "CLOSED or filtered", (double)s_ui.port_ms);
            break;
        default: break;
    }
    lv_label_set_text(s_ui.out, s_ui.result);
    lv_obj_scroll_to_y(s_ui.out_box, 0, LV_ANIM_OFF);
    s_ui.job = -1;
    ping_set_busy(false);
}

static bool take_host(void)
{
    const char *t = lv_textarea_get_text(s_ui.host_ta);
    while (*t == ' ') t++;
    if (*t == '\0') {
        lv_label_set_text(s_ui.out, "Type a host name or address first.");
        return false;
    }
    snprintf(s_ui.host, sizeof(s_ui.host), "%s", t);
    for (char *p = s_ui.host; *p; p++) {
        if (*p == ' ' || *p == '/') {
            *p = '\0';
            break;
        }
    }
    return true;
}

static void run_job(int job)
{
    if (net_tools_busy() || !take_host()) return;
    if (net_state() != NET_CONNECTED) {
        lv_label_set_text(s_ui.out, "No network. Join Wi-Fi in SYSTEM first.");
        return;
    }
    s_ui.job = job;
    static const char *const names[] = {"PINGING", "RESOLVING", "TRACING", "CHECKING PORT ON"};
    lv_label_set_text_fmt(s_ui.out, "%s %s...", names[job], s_ui.host);
    ping_set_busy(true);
    if (!net_tools_run(ping_job, ping_done, NULL)) {
        lv_label_set_text(s_ui.out, "Could not start the job.");
        s_ui.job = -1;
        ping_set_busy(false);
    }
}

static void port_entered(const char *text, void *u)
{
    (void)u;
    if (text == NULL) return;
    int port = atoi(text);
    if (port < 1 || port > 65535) {
        lv_label_set_text(s_ui.out, "Port must be 1 to 65535.");
        return;
    }
    s_ui.port = port;
    run_job(JOB_PORT);
}

static void ping_btn_clicked(lv_event_t *e)
{
    int job = (int)(intptr_t)lv_event_get_user_data(e);
    if (job == JOB_PORT) {
        if (!take_host()) return;
        char cur[8];
        snprintf(cur, sizeof(cur), "%d", s_ui.port ? s_ui.port : 443);
        deck_modal_prompt("PORT", cur, port_entered, NULL);
        return;
    }
    run_job(job);
}

static void host_enter(lv_event_t *e)
{
    (void)e;
    run_job(JOB_PING);
}

static void build_ping(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 4, 0);

    lv_obj_t *in = deck_panel(page, DECK_CUT_BR, 6);
    lv_obj_set_size(in, LV_PCT(100), LV_SIZE_CONTENT);
    deck_panel_set_outline(in, g_pal.accent);
    lv_obj_set_style_pad_hor(in, 6, 0);
    lv_obj_set_style_pad_ver(in, 4, 0);
    s_ui.host_ta = lv_textarea_create(in);
    lv_textarea_set_one_line(s_ui.host_ta, true);
    lv_textarea_set_placeholder_text(s_ui.host_ta, "host name or address");
    lv_textarea_set_max_length(s_ui.host_ta, 90);
    lv_obj_set_width(s_ui.host_ta, LV_PCT(100));
    lv_obj_set_style_text_font(s_ui.host_ta, g_font.mono_m, 0);
    lv_obj_set_style_text_color(s_ui.host_ta, g_pal.text, 0);
    lv_obj_set_style_bg_opa(s_ui.host_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.host_ta, 0, 0);
    lv_obj_set_style_pad_all(s_ui.host_ta, 0, 0);
    lv_obj_set_style_bg_color(s_ui.host_ta, g_pal.accent, LV_PART_CURSOR);
    lv_obj_set_style_text_color(s_ui.host_ta, g_pal.dim, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_ui.host_ta, host_enter, LV_EVENT_READY, NULL);

    lv_obj_t *row = deck_box(page);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    static const char *const labels[] = {"PING", "DNS", "TRACE", "PORT"};
    for (int i = 0; i < 4; i++) {
        s_ui.ping_btn[i] = small_button(row, labels[i], ping_btn_clicked, i);
        lv_obj_set_flex_grow(s_ui.ping_btn[i], 1);
    }

    s_ui.out_box = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 6);
    lv_obj_set_width(s_ui.out_box, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.out_box, 1);
    lv_obj_set_style_pad_hor(s_ui.out_box, 6, 0);
    lv_obj_set_style_pad_ver(s_ui.out_box, 4, 0);
    lv_obj_add_flag(s_ui.out_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.out_box, LV_DIR_VER);
    s_ui.out = deck_label(s_ui.out_box, g_font.mono_s, g_pal.text,
                          "Type a host, then ENTER to ping,\nor tap DNS, TRACE or PORT.");
    lv_obj_set_width(s_ui.out, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_ui.out, 2, 0);
}

/* ---- LAN ----------------------------------------------------------------- */

static void lan_progress(int done, int total, void *u)
{
    (void)u;
    s_ui.lan_done  = done;
    s_ui.lan_total = total;
}

static void lan_job(void *u)
{
    (void)u;
    s_ui.host_n = net_lan_scan(s_ui.hosts, HOSTS_MAX, lan_progress, NULL);
}

static void render_lan(void)
{
    lv_obj_clean(s_ui.lan_list);
    if (s_ui.host_n == 0) {
        deck_label(s_ui.lan_list, g_font.mono_s, g_pal.dim,
                   s_ui.lan_running ? "Sweeping the subnet..." : "Ping sweep of your /24 plus mDNS\nnames. Press SCAN or R.");
        return;
    }
    for (int i = 0; i < s_ui.host_n; i++) {
        const net_host_t *h = &s_ui.hosts[i];
        lv_obj_t *row       = deck_panel(s_ui.lan_list, DECK_CUT_BR, 5);
        lv_obj_set_size(row, LV_PCT(100), 32);
        lv_obj_set_style_pad_hor(row, 5, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        if (h->self) deck_panel_set_outline(row, g_pal.accent);

        lv_obj_t *ip = deck_label(row, g_font.mono_s, h->self ? g_pal.accent : g_pal.text, h->ip);
        lv_obj_align(ip, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *name = deck_label(row, g_font.mono_s, h->name[0] ? g_pal.text : g_pal.dim,
                                    h->self ? "this deck" : h->name[0] ? h->name : "-");
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, 190);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(name, LV_ALIGN_TOP_RIGHT, 0, 0);

        const char *vendor = h->has_mac ? net_oui_vendor(h->mac) : "";
        char mac[48] = "";
        if (h->has_mac) {
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X%s%s", h->mac[0], h->mac[1], h->mac[2], h->mac[3],
                     h->mac[4], h->mac[5], vendor[0] ? " " : "", vendor);
        }
        lv_obj_t *m = deck_label(row, g_font.mono_s, g_pal.dim, mac);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(m, 220);
        lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        char svc[32];
        if (h->service[0]) {
            snprintf(svc, sizeof(svc), "%s", h->service);
        } else if (h->ms > 0) {
            snprintf(svc, sizeof(svc), "%.0f ms", (double)h->ms);
        } else {
            svc[0] = '\0';
        }
        lv_obj_t *s = deck_label(row, g_font.mono_s, g_pal.accent2, svc);
        lv_obj_align(s, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}

static void lan_done_cb(void *u)
{
    (void)u;
    if (s_ui.lan_list == NULL) return;
    s_ui.lan_running = false;
    lv_label_set_text(lv_obj_get_child(s_ui.lan_btn, 0), "SCAN");
    lv_obj_remove_state(s_ui.lan_btn, LV_STATE_DISABLED);
    lv_label_set_text_fmt(s_ui.lan_status, "%d DEVICE%s FOUND", s_ui.host_n, s_ui.host_n == 1 ? "" : "S");
    render_lan();
}

static void lan_tick(void)
{
    if (!s_ui.lan_running || s_ui.lan_status == NULL) return;
    int total = s_ui.lan_total ? s_ui.lan_total : 1;
    lv_label_set_text_fmt(s_ui.lan_status, "SWEEPING  %d%%", s_ui.lan_done * 100 / total);
}

static void lan_scan_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.lan_running || net_tools_busy()) return;
    if (net_state() != NET_CONNECTED) {
        lv_label_set_text(s_ui.lan_status, "NO NETWORK");
        return;
    }
    s_ui.lan_running = true;
    s_ui.lan_done = s_ui.lan_total = 0;
    s_ui.host_n                    = 0;
    lv_label_set_text(lv_obj_get_child(s_ui.lan_btn, 0), "...");
    lv_obj_add_state(s_ui.lan_btn, LV_STATE_DISABLED);
    lv_label_set_text(s_ui.lan_status, "SWEEPING  0%");
    render_lan();
    if (!net_tools_run(lan_job, lan_done_cb, NULL)) {
        s_ui.lan_running = false;
        lv_label_set_text(lv_obj_get_child(s_ui.lan_btn, 0), "SCAN");
        lv_obj_remove_state(s_ui.lan_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_ui.lan_status, "BUSY, TRY AGAIN");
    }
}

static lv_obj_t *action_row(lv_obj_t *page, lv_obj_t **status, const char *text, lv_obj_t **btn, const char *label,
                            lv_event_cb_t cb)
{
    lv_obj_t *row = deck_box(page);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    *status = deck_label(row, g_font.mono_s, g_pal.dim, text);
    lv_obj_align(*status, LV_ALIGN_LEFT_MID, 0, 0);
    *btn = small_button(row, label, cb, 0);
    lv_obj_align(*btn, LV_ALIGN_RIGHT_MID, 0, 0);
    return row;
}

static void build_lan(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 4, 0);
    action_row(page, &s_ui.lan_status, "WHO IS ON THIS NETWORK", &s_ui.lan_btn, "SCAN", lan_scan_clicked);
    s_ui.lan_list = scroll_list(page);
    render_lan();
}

/* ---- SPEED --------------------------------------------------------------- */

static void speed_job(void *u)
{
    (void)u;
    s_ui.spd_phase   = 1;
    s_ui.spd_lat_ok  = net_port_open(SPEED_HOST, 443, 3000, &s_ui.spd_lat_ms);
    s_ui.spd_phase   = 2;
    s_ui.spd_down_ok = net_speed_download(SPEED_DOWN_URL, SPEED_DOWN_BYTES, 20000, &s_ui.spd_down_bytes, &s_ui.spd_down_ms);
    s_ui.spd_phase   = 3;
    s_ui.spd_up_ok   = net_speed_upload(SPEED_UP_URL, SPEED_UP_BYTES, 20000, &s_ui.spd_up_ms);
    s_ui.spd_phase   = 4;
}

static void mbps(char *out, size_t n, size_t bytes, float ms, bool ok)
{
    if (!ok || ms <= 0) {
        snprintf(out, n, "--");
        return;
    }
    double v = (double)bytes * 8.0 / (ms / 1000.0) / 1e6;
    snprintf(out, n, v >= 100 ? "%.0f" : "%.1f", v);
}

static void speed_done(void *u)
{
    (void)u;
    if (s_ui.spd_btn == NULL) return;
    char d[16], up[16];
    mbps(d, sizeof(d), s_ui.spd_down_bytes, s_ui.spd_down_ms, s_ui.spd_down_ok);
    mbps(up, sizeof(up), SPEED_UP_BYTES, s_ui.spd_up_ms, s_ui.spd_up_ok);
    lv_label_set_text(s_ui.spd_down, d);
    lv_label_set_text(s_ui.spd_up, up);
    if (s_ui.spd_lat_ok) {
        lv_label_set_text_fmt(s_ui.spd_lat, "%.0f", (double)s_ui.spd_lat_ms);
    } else {
        lv_label_set_text(s_ui.spd_lat, "--");
    }
    lv_label_set_text(s_ui.spd_status, s_ui.spd_down_ok ? "DONE" : "FAILED: NO ANSWER FROM SERVER");
    lv_label_set_text(lv_obj_get_child(s_ui.spd_btn, 0), "START");
    lv_obj_remove_state(s_ui.spd_btn, LV_STATE_DISABLED);
    s_ui.spd_phase = 0;
}

static void speed_tick(void)
{
    if (s_ui.spd_status == NULL || s_ui.spd_phase == 0 || s_ui.spd_phase == 4) return;
    static const char *const names[] = {"", "LATENCY...", "DOWNLOADING 8 MB...", "UPLOADING 2 MB...", ""};
    lv_label_set_text(s_ui.spd_status, names[s_ui.spd_phase]);
}

static void speed_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.spd_phase != 0 || net_tools_busy()) return;
    if (net_state() != NET_CONNECTED) {
        lv_label_set_text(s_ui.spd_status, "NO NETWORK");
        return;
    }
    lv_label_set_text(s_ui.spd_down, "...");
    lv_label_set_text(s_ui.spd_up, "...");
    lv_label_set_text(s_ui.spd_lat, "...");
    lv_label_set_text(lv_obj_get_child(s_ui.spd_btn, 0), "...");
    lv_obj_add_state(s_ui.spd_btn, LV_STATE_DISABLED);
    s_ui.spd_phase = 1;
    speed_tick();
    if (!net_tools_run(speed_job, speed_done, NULL)) {
        s_ui.spd_phase = 0;
        lv_label_set_text(lv_obj_get_child(s_ui.spd_btn, 0), "START");
        lv_obj_remove_state(s_ui.spd_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_ui.spd_status, "BUSY, TRY AGAIN");
    }
}

static void speed_cell(lv_obj_t *parent, const char *title, const char *unit, lv_obj_t **value)
{
    lv_obj_t *p = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 8);
    lv_obj_set_height(p, LV_PCT(100));
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_pad_all(p, 2, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    deck_label(p, g_font.disp_s, g_pal.accent, title);
    *value = deck_label(p, g_font.disp_xl, g_pal.text, "--");
    deck_label(p, g_font.mono_s, g_pal.dim, unit);
}

static void build_speed(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 4, 0);
    action_row(page, &s_ui.spd_status, "8 MB DOWN, 2 MB UP", &s_ui.spd_btn, "START", speed_clicked);

    lv_obj_t *cells = deck_box(page);
    lv_obj_set_width(cells, LV_PCT(100));
    lv_obj_set_flex_grow(cells, 1);
    lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cells, 4, 0);
    speed_cell(cells, "DOWN", "Mbit/s", &s_ui.spd_down);
    speed_cell(cells, "UP", "Mbit/s", &s_ui.spd_up);
    speed_cell(cells, "PING", "ms", &s_ui.spd_lat);

    lv_obj_t *src = deck_label(page, g_font.mono_s, g_pal.dim, "thinkbroadband (HTTP) / Cloudflare");
    lv_obj_set_width(src, LV_PCT(100));
    lv_obj_set_style_text_align(src, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- Tabs ---------------------------------------------------------------- */

static void set_tab(int tab)
{
    s_ui.tab = tab;
    for (int i = 0; i < TABS; i++) {
        if (i == tab) {
            lv_obj_remove_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(s_ui.tab_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(s_ui.tab_btn[i], LV_STATE_CHECKED);
        }
        lv_obj_send_event(s_ui.tab_btn[i], LV_EVENT_STATE_CHANGED, NULL);
    }
    lv_group_remove_all_objs(deck_input_group());
    set_status("");
    if (tab == TAB_SCAN) {
        render_scan();
        render_scan_status();
    } else if (tab == TAB_PING) {
        lv_group_add_obj(deck_input_group(), s_ui.host_ta);
        for (int i = 0; i < 4; i++) lv_group_add_obj(deck_input_group(), s_ui.ping_btn[i]);
        lv_group_focus_obj(s_ui.host_ta);
        set_status(net_ip()[0] ? net_ip() : "OFFLINE");
    } else if (tab == TAB_LAN) {
        lv_group_add_obj(deck_input_group(), s_ui.lan_btn);
        set_status(net_ip()[0] ? net_ip() : "OFFLINE");
    } else if (tab == TAB_SPEED) {
        lv_group_add_obj(deck_input_group(), s_ui.spd_btn);
        set_status(net_ssid()[0] ? net_ssid() : "OFFLINE");
    }
}

static void tab_clicked(lv_event_t *e) { set_tab((int)(intptr_t)lv_event_get_user_data(e)); }

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.job = -1;

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(root, 6, 0);
    lv_obj_set_style_pad_ver(root, 4, 0);
    lv_obj_set_style_pad_row(root, 4, 0);

    lv_obj_t *bar = deck_box(root);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 3, 0);
    for (int i = 0; i < TABS; i++) s_ui.tab_btn[i] = small_button(bar, s_tab_names[i], tab_clicked, i);
    s_ui.status = deck_label(bar, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_flex_grow(s_ui.status, 1);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *content = deck_box(root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    for (int i = 0; i < TABS; i++) {
        s_ui.page[i] = deck_box(content);
        lv_obj_set_size(s_ui.page[i], LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
    }
    build_scan(s_ui.page[TAB_SCAN]);
    build_ping(s_ui.page[TAB_PING]);
    build_lan(s_ui.page[TAB_LAN]);
    build_speed(s_ui.page[TAB_SPEED]);

    s_ui.timer = lv_timer_create(tick, 1000, NULL);
    set_tab(TAB_SCAN);
    start_scan();
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    /* A job still running writes into s_ui and its done callback checks the
     * widget pointers, which this clears. */
    memset(&s_ui, 0, sizeof(s_ui));
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    /* On the PING tab the digits and letters belong to the host field. */
    bool typing = s_ui.tab == TAB_PING && lv_group_get_focused(deck_input_group()) == s_ui.host_ta;
    if (!typing && k->code >= HID_1 && k->code < HID_1 + TABS) {
        set_tab(k->code - HID_1);
        return true;
    }
    if (typing || k->code != HID_R) return false;
    switch (s_ui.tab) {
        case TAB_SCAN:
            s_ui.scanned_at = 0;
            start_scan();
            return true;
        case TAB_LAN: lan_scan_clicked(NULL); return true;
        case TAB_SPEED: speed_clicked(NULL); return true;
        default: return false;
    }
}

static deck_app_t s_app = {
    .name     = "NET TOOLS",
    .tagline  = "WI-FI + LAN",
    .icon     = ICON_LAN,
    .eta      = NULL,
    .hotkey   = HID_LETTER('R'),
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_net(void) { return &s_app; }
