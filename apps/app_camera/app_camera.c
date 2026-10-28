/*
 * CAMERA module: a viewfinder for an M5Stack UnitCamS3 (or UnitCamS3-5MP)
 * on the Grove port. The Grove only feeds it power; pictures come over
 * Wi-Fi from the unit's stock firmware (http://<host>/api/v1/...). Either
 * join the unit's own "UnitCAMS3-WiFi" network from SYSTEM (host
 * 192.168.4.1) or point the unit at your network from its web page and
 * set its address here.
 *
 *   LIVE     frames pulled from /api/v1/capture at 320x240 as fast as the
 *            link allows; Enter or Space takes a photo at the chosen size
 *            and saves it to <storage>/photos as a JPEG
 *   GALLERY  the saved photos, newest first; Enter views, D deletes
 *
 * Keys: Enter/Space shoot, S photo size, G gallery, H camera host,
 * A hop onto the camera's own Wi-Fi and back home, W that network's
 * password, L camera LED, B swap colour bytes (if the picture looks wrong).
 * In the gallery: Up/Down pick, Enter view, D delete, Left/Right next.
 */
#include "app_camera.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cam_jpeg.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define HID_A 0x04
#define HID_B 0x05
#define HID_D 0x07
#define HID_G 0x0A
#define HID_L 0x0F
#define HID_S 0x16
#define HID_W 0x1A

#define LIVE_FRAMESIZE 5 /* esp32-camera FRAMESIZE_QVGA, 320x240 */
#define CAM_AP_SSID    "UnitCAMS3-WiFi" /* the unit's own access point */
#define CAM_AP_HOST    "192.168.4.1"
#define VIEW_W         HAL_SCREEN_W
#define VIEW_H         (HAL_SCREEN_H - DECK_STATUSBAR_H - 22) /* minus the tool row */
#define MAX_PHOTOS     200
#define PNAME_MAX       40
#define PHOTO_MAX      (1200 * 1024) /* largest file the viewer will load */
#define RETRY_MS       2500
#define FRAME_GAP_MS   100 /* breathing room for the UI between frames */

typedef struct {
    const char *label;
    int framesize; /* esp32-camera framesize_t */
} size_opt_t;

static const size_opt_t s_sizes[] = {
    {"VGA 640x480", 8},   {"SVGA 800x600", 9},   {"XGA 1024x768", 10},
    {"HD 1280x720", 11},  {"UXGA 1600x1200", 13}, {"5MP 2592x1944", 18},
};
#define SIZE_COUNT ((int)(sizeof(s_sizes) / sizeof(s_sizes[0])))

typedef enum { VIEW_LIVE, VIEW_GALLERY, VIEW_PHOTO } view_t;

static struct {
    lv_obj_t *parent, *screen;
    view_t view;

    /* live view */
    lv_obj_t *img, *status, *size_btn, *shot_btn;
    lv_image_dsc_t dsc;
    uint16_t *pix;
    volatile bool alive; /* net_fetch results are dropped once this is false */
    bool live;           /* the frame loop should keep going */
    bool busy;           /* a request is in flight */
    bool shoot_req;      /* take a photo after the current frame */
    int shot_step;       /* 0 idle, 1 size set, 2 captured, 3 size restored */
    lv_timer_t *retry;
    uint32_t frames, fps_t0;
    uint32_t status_hold; /* tick until which frame stats leave the status alone */
    int fps, fails;
    bool led;

    /* settings */
    char host[64];
    int size_idx;
    bool swap;
    lv_obj_t *ap_btn;

    /* gallery */
    char names[MAX_PHOTOS][PNAME_MAX];
    int count, sel;
    lv_obj_t *list, *rows[MAX_PHOTOS];
    lv_obj_t *photo_img, *photo_name;
    uint16_t *photo_pix;
} s_c;

static void show_live(void);
static void show_gallery(int sel);
static void show_photo(int index);
static void live_next(void);
static bool on_cam_ap(void);

/* ---- Small helpers ------------------------------------------------------- */

static lv_obj_t *page(void)
{
    if (s_c.screen) {
        lv_obj_add_flag(s_c.screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_c.screen);
    }
    lv_group_remove_all_objs(deck_input_group());
    s_c.screen = deck_box(s_c.parent);
    lv_obj_set_size(s_c.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_c.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_c.screen, 0, 0);
    s_c.img = s_c.status = s_c.size_btn = s_c.shot_btn = s_c.ap_btn = NULL;
    s_c.list = s_c.photo_img = s_c.photo_name = NULL;
    return s_c.screen;
}

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
    lv_obj_set_height(bar, 22);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_style_pad_hor(bar, 3, 0);
    return bar;
}

static void set_status(const char *text, lv_color_t color)
{
    if (s_c.status == NULL) return;
    lv_label_set_text(s_c.status, text);
    lv_obj_set_style_text_color(s_c.status, color, 0);
}

/* Same, but stays up for a couple of seconds over the frame counter. */
static void hold_status(const char *text, lv_color_t color)
{
    set_status(text, color);
    s_c.status_hold = lv_tick_get() + 2500;
}

static void url_for(char *out, size_t n, const char *path)
{
    snprintf(out, n, "http://%s%s", s_c.host, path);
}

/* Point an lv_image at a fresh RGB565 buffer and free the one it showed. */
static void show_pixels(lv_obj_t *img, lv_image_dsc_t *dsc, uint16_t **slot, uint16_t *pix, int w, int h)
{
    if (img == NULL) {
        free(pix);
        return;
    }
    lv_image_cache_drop(dsc);
    uint16_t *old = *slot;
    *slot = pix;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf    = LV_COLOR_FORMAT_RGB565;
    dsc->header.w     = (uint32_t)w;
    dsc->header.h     = (uint32_t)h;
    dsc->header.stride = (uint32_t)w * 2;
    dsc->data_size    = (uint32_t)w * (uint32_t)h * 2;
    dsc->data         = (const uint8_t *)pix;
    lv_image_set_src(img, dsc);
    lv_obj_invalidate(img);
    free(old); /* rendering is synchronous on this task, the old frame is done */
}

/* ---- Photos on storage --------------------------------------------------- */

static bool photos_dir(char *out, size_t n)
{
    const char *root = hal_storage_root();
    if (root == NULL) return false;
    snprintf(out, n, "%s/photos", root);
    struct stat st;
    if (stat(out, &st) != 0 && mkdir(out, 0755) != 0) return false;
    return true;
}

static int by_name_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static void photos_scan(void)
{
    s_c.count = 0;
    char dir[128];
    if (!photos_dir(dir, sizeof(dir))) return;
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_c.count < MAX_PHOTOS) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n >= PNAME_MAX || n < 5) continue;
        if (strcasecmp(e->d_name + n - 4, ".jpg") != 0) continue;
        snprintf(s_c.names[s_c.count++], PNAME_MAX, "%s", e->d_name);
    }
    closedir(d);
    /* Names carry the timestamp, so newest first is a reverse sort. */
    qsort(s_c.names, (size_t)s_c.count, PNAME_MAX, by_name_desc);
}

static bool photo_save(const char *body, size_t len, char *name, size_t n)
{
    char dir[128], path[200];
    if (!photos_dir(dir, sizeof(dir))) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2020) {
        strftime(name, n, "CAM_%Y%m%d_%H%M%S.jpg", &tm);
    } else {
        snprintf(name, n, "CAM_%08lu.jpg", (unsigned long)(lv_tick_get() / 10));
    }
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    bool ok = fwrite(body, 1, len, f) == len;
    ok      = (fclose(f) == 0) && ok;
    if (!ok) unlink(path);
    return ok;
}

static char *photo_load(const char *name, size_t *len)
{
    char dir[128], path[200];
    if (!photos_dir(dir, sizeof(dir))) return NULL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > PHOTO_MAX) {
        fclose(f);
        return NULL;
    }
    char *buf  = malloc((size_t)size);
    size_t got = buf ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    if (buf == NULL || got != (size_t)size) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

static void photo_delete(const char *name)
{
    char dir[128], path[200];
    if (!photos_dir(dir, sizeof(dir))) return;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
}

/* ---- Live view ----------------------------------------------------------- */

static void size_label(char *out, size_t n)
{
    snprintf(out, n, "%s", s_sizes[s_c.size_idx].label);
    char *sp = strchr(out, ' '); /* the chip shows just "VGA" */
    if (sp) *sp = '\0';
}

static void retry_cb(lv_timer_t *t)
{
    (void)t;
    s_c.retry = NULL;
    live_next();
}

static void schedule_next(uint32_t ms)
{
    if (s_c.retry) lv_timer_delete(s_c.retry);
    s_c.retry = lv_timer_create(retry_cb, ms, NULL);
    if (s_c.retry) lv_timer_set_repeat_count(s_c.retry, 1);
}

static void schedule_retry(void) { schedule_next(RETRY_MS); }

static void shot_step(void);

static void frame_cb(char *body, size_t len, void *user)
{
    (void)user;
    s_c.busy = false;
    if (s_c.view != VIEW_LIVE) return; /* user moved on */
    if (body == NULL) {
        s_c.fails++;
        char msg[96];
        snprintf(msg, sizeof(msg), on_cam_ap() ? "NO CAMERA AT %s" : "NO CAMERA AT %s (CAM AP?)", s_c.host);
        set_status(msg, g_pal.warn);
        schedule_retry();
        return;
    }
    s_c.fails = 0;
    uint16_t *pix;
    int w, h;
    if (cam_jpeg_decode((const uint8_t *)body, len, 0, 0, &pix, &w, &h)) {
        show_pixels(s_c.img, &s_c.dsc, &s_c.pix, pix, w, h);
        s_c.frames++;
        uint32_t el = lv_tick_elaps(s_c.fps_t0);
        if (el >= 1000) {
            s_c.fps    = (int)(s_c.frames * 1000 / el);
            s_c.frames = 0;
            s_c.fps_t0 = lv_tick_get();
        }
        if (lv_tick_get() >= s_c.status_hold) {
            char msg[48];
            snprintf(msg, sizeof(msg), "LIVE %dx%d  %d FPS  %lu KB", w, h, s_c.fps, (unsigned long)(len / 1024));
            set_status(msg, g_pal.accent);
        }
    } else {
        set_status("BAD FRAME", g_pal.warn);
    }
    if (s_c.shoot_req) {
        s_c.shoot_req = false;
        shot_step();
    } else {
        schedule_next(FRAME_GAP_MS);
    }
}

static void live_next(void)
{
    if (!s_c.live || s_c.busy || s_c.shot_step || s_c.view != VIEW_LIVE) return;
    if (net_state() != NET_CONNECTED) {
        set_status("NO NETWORK: CAM AP JOINS THE UNIT'S WI-FI", g_pal.warn);
        schedule_retry();
        return;
    }
    char url[160];
    url_for(url, sizeof(url), "/api/v1/capture");
    s_c.busy = true;
    net_fetch(url, frame_cb, NULL, &s_c.alive);
}

/* Taking a photo: set the photo size, capture, put the live size back. */
static void shot_cb(char *body, size_t len, void *user)
{
    (void)user;
    s_c.busy = false;
    if (s_c.view != VIEW_LIVE) {
        s_c.shot_step = 0;
        return;
    }
    if (s_c.shot_step == 2) {
        char name[PNAME_MAX];
        if (body == NULL) {
            hold_status("CAPTURE FAILED", g_pal.warn);
        } else if (photo_save(body, len, name, sizeof(name))) {
            char msg[80];
            snprintf(msg, sizeof(msg), "SAVED %s  %lu KB", name, (unsigned long)(len / 1024));
            hold_status(msg, g_pal.ok);
            uint16_t *pix;
            int w, h;
            if (cam_jpeg_decode((const uint8_t *)body, len, VIEW_W, HAL_SCREEN_H, &pix, &w, &h)) {
                show_pixels(s_c.img, &s_c.dsc, &s_c.pix, pix, w, h); /* a glimpse of the real thing */
            }
        } else {
            hold_status(hal_storage_root() ? "SAVE FAILED" : "NO STORAGE FOR PHOTOS", g_pal.warn);
        }
    }
    shot_step();
}

static void shot_step(void)
{
    char url[160], path[80];
    s_c.shot_step++;
    switch (s_c.shot_step) {
        case 1:
            set_status("SHOOTING...", g_pal.text);
            snprintf(path, sizeof(path), "/api/v1/control?var=framesize&val=%d", s_sizes[s_c.size_idx].framesize);
            break;
        case 2: snprintf(path, sizeof(path), "/api/v1/capture"); break;
        case 3: snprintf(path, sizeof(path), "/api/v1/control?var=framesize&val=%d", LIVE_FRAMESIZE); break;
        default:
            s_c.shot_step = 0;
            live_next();
            return;
    }
    url_for(url, sizeof(url), path);
    s_c.busy = true;
    net_fetch(url, shot_cb, NULL, &s_c.alive);
}

static void shoot(void)
{
    if (s_c.view != VIEW_LIVE || s_c.shot_step) return;
    if (s_c.busy) {
        s_c.shoot_req = true; /* after the frame in flight */
    } else {
        shot_step();
    }
}

static void size_cycle(void)
{
    s_c.size_idx = (s_c.size_idx + 1) % SIZE_COUNT;
    hal_cfg_set_i32("cam_size", s_c.size_idx);
    if (s_c.size_btn) {
        char l[16];
        size_label(l, sizeof(l));
        lv_label_set_text(lv_obj_get_child(s_c.size_btn, 0), l);
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "PHOTO SIZE %s", s_sizes[s_c.size_idx].label);
    hold_status(msg, g_pal.text);
}

static void led_cb(char *body, size_t len, void *user)
{
    (void)body;
    (void)len;
    (void)user;
}

static void led_toggle(void)
{
    s_c.led = !s_c.led;
    char url[160];
    url_for(url, sizeof(url), s_c.led ? "/api/v1/led_on" : "/api/v1/led_off");
    net_fetch(url, led_cb, NULL, &s_c.alive);
    hold_status(s_c.led ? "CAMERA LED ON" : "CAMERA LED OFF", g_pal.text);
}

static void swap_toggle(void)
{
    s_c.swap = !s_c.swap;
    hal_cfg_set_i32("cam_swap", s_c.swap);
    cam_jpeg_set_swap(s_c.swap);
    hold_status(s_c.swap ? "COLOUR BYTES SWAPPED" : "COLOUR BYTES NORMAL", g_pal.text);
}

static void host_set(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == '\0') return;
    snprintf(s_c.host, sizeof(s_c.host), "%s", text);
    hal_cfg_set_str("cam_host", s_c.host);
    s_c.fails = 0;
    show_live();
}

static void host_prompt(void) { deck_modal_prompt("CAMERA HOST OR IP", s_c.host, host_set, NULL); }

/* The unit arrives as its own access point. Hopping onto it from here
 * saves the home network first, so the same button brings you back. */
static bool on_cam_ap(void)
{
    char ssid[33] = "";
    hal_cfg_get_str("cam_ap_ssid", ssid, sizeof(ssid));
    if (ssid[0] == '\0') snprintf(ssid, sizeof(ssid), CAM_AP_SSID);
    return strcmp(net_ssid(), ssid) == 0;
}

static void ap_button_label(void)
{
    if (s_c.ap_btn) lv_label_set_text(lv_obj_get_child(s_c.ap_btn, 0), on_cam_ap() ? "HOME" : "CAM AP");
}

static void ap_hop(void)
{
    char ssid[33] = "", pass[65] = "";
    if (on_cam_ap()) {
        hal_cfg_get_str("cam_home_ssid", ssid, sizeof(ssid));
        hal_cfg_get_str("cam_home_pass", pass, sizeof(pass));
        if (ssid[0] == '\0') {
            hold_status("NO HOME NETWORK SAVED: USE SYSTEM > JOIN", g_pal.warn);
            return;
        }
        net_connect(ssid, pass);
        hold_status("JOINING HOME WI-FI...", g_pal.text);
    } else {
        if (net_ssid()[0]) {
            hal_cfg_get_str("wifi_pass", pass, sizeof(pass));
            hal_cfg_set_str("cam_home_ssid", net_ssid());
            hal_cfg_set_str("cam_home_pass", pass);
        }
        hal_cfg_get_str("cam_ap_ssid", ssid, sizeof(ssid));
        if (ssid[0] == '\0') snprintf(ssid, sizeof(ssid), CAM_AP_SSID);
        pass[0] = '\0';
        hal_cfg_get_str("cam_ap_pass", pass, sizeof(pass));
        net_connect(ssid, pass);
        snprintf(s_c.host, sizeof(s_c.host), CAM_AP_HOST);
        hal_cfg_set_str("cam_host", s_c.host);
        char msg[80];
        snprintf(msg, sizeof(msg), "JOINING %s...", ssid);
        hold_status(msg, g_pal.text);
    }
    s_c.fails = 0;
    ap_button_label();
    schedule_retry();
}

static void ap_pass_set(const char *text, void *user)
{
    (void)user;
    hal_cfg_set_str("cam_ap_pass", text && text[0] ? text : NULL);
    hold_status("CAMERA WI-FI PASSWORD SAVED", g_pal.text);
}

static void ap_pass_prompt(void) { deck_modal_password("CAMERA WI-FI PASSWORD (BLANK = OPEN)", ap_pass_set, NULL); }

static void ap_clicked(lv_event_t *e)
{
    (void)e;
    ap_hop();
}

static void shot_clicked(lv_event_t *e)
{
    (void)e;
    shoot();
}
static void size_clicked(lv_event_t *e)
{
    (void)e;
    size_cycle();
}
static void gallery_clicked(lv_event_t *e)
{
    (void)e;
    show_gallery(0);
}
static void host_clicked(lv_event_t *e)
{
    (void)e;
    host_prompt();
}
static void led_clicked(lv_event_t *e)
{
    (void)e;
    led_toggle();
}

static void setup_cb(char *body, size_t len, void *user)
{
    (void)body;
    (void)len;
    (void)user;
    s_c.busy = false;
    live_next();
}

static void show_live(void)
{
    s_c.view = VIEW_LIVE;
    lv_obj_t *root = page();

    /* Viewfinder: the 320x240 frame sits behind a 320x196 window, so the
     * middle shows at full size and nothing is scaled every frame. */
    lv_obj_t *frame = deck_box(root);
    lv_obj_set_size(frame, VIEW_W, VIEW_H);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    s_c.img = lv_image_create(frame);
    lv_obj_set_size(s_c.img, VIEW_W, VIEW_H);
    lv_image_set_inner_align(s_c.img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(s_c.img);
    if (s_c.pix) lv_image_set_src(s_c.img, &s_c.dsc); /* last frame while the next loads */

    s_c.status = deck_label(frame, g_font.mono_s, g_pal.accent, "CONNECTING...");
    lv_obj_align(s_c.status, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_width(s_c.status, VIEW_W - 8);
    lv_label_set_long_mode(s_c.status, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_bg_color(s_c.status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_c.status, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_c.status, 3, 0);

    lv_obj_t *bar = toolbar(root);
    s_c.shot_btn  = tool_button(bar, "SHOT", shot_clicked, NULL);
    deck_panel_set_outline(s_c.shot_btn, g_pal.accent);
    char l[16];
    size_label(l, sizeof(l));
    s_c.size_btn = tool_button(bar, l, size_clicked, NULL);
    tool_button(bar, "LED", led_clicked, NULL);
    lv_obj_t *spacer = deck_box(bar);
    lv_obj_set_flex_grow(spacer, 1);
    tool_button(bar, ICON_IMAGE, gallery_clicked, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(bar, (int32_t)lv_obj_get_child_count(bar) - 1), 0),
                               g_font.icon_s, 0);
    s_c.ap_btn = tool_button(bar, "CAM AP", ap_clicked, NULL);
    ap_button_label();
    tool_button(bar, "HOST", host_clicked, NULL);

    /* Start: ask for the live size, then stream. */
    s_c.live   = true;
    s_c.frames = 0;
    s_c.fps_t0 = lv_tick_get();
    if (net_state() == NET_CONNECTED && !s_c.busy && !s_c.shot_step) {
        char url[160], path[80];
        snprintf(path, sizeof(path), "/api/v1/control?var=framesize&val=%d", LIVE_FRAMESIZE);
        url_for(url, sizeof(url), path);
        s_c.busy = true;
        net_fetch(url, setup_cb, NULL, &s_c.alive);
    } else {
        live_next();
    }
}

/* ---- Gallery ------------------------------------------------------------- */

static void row_clicked(lv_event_t *e)
{
    show_photo((int)(intptr_t)lv_event_get_user_data(e));
}

static void live_clicked(lv_event_t *e)
{
    (void)e;
    show_live();
}

static void gallery_select(int sel)
{
    if (s_c.count == 0) return;
    if (sel < 0) sel = 0;
    if (sel >= s_c.count) sel = s_c.count - 1;
    if (s_c.rows[s_c.sel]) deck_panel_set_outline(s_c.rows[s_c.sel], g_pal.dim);
    s_c.sel = sel;
    if (s_c.rows[sel]) {
        deck_panel_set_outline(s_c.rows[sel], g_pal.accent);
        lv_obj_scroll_to_view(s_c.rows[sel], LV_ANIM_OFF);
    }
}

static void show_gallery(int sel)
{
    s_c.view = VIEW_GALLERY;
    s_c.live = false;
    photos_scan();
    lv_obj_t *root = page();

    lv_obj_t *head = toolbar(root);
    tool_button(head, "LIVE", live_clicked, NULL);
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// PHOTOS");
    (void)t;
    char n[32];
    snprintf(n, sizeof(n), "%d", s_c.count);
    deck_label(head, g_font.mono_s, g_pal.dim, n);

    s_c.list = deck_box(root);
    lv_obj_set_width(s_c.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_c.list, 1);
    lv_obj_set_flex_flow(s_c.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_c.list, 2, 0);
    lv_obj_set_style_pad_all(s_c.list, 3, 0);
    lv_obj_add_flag(s_c.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_c.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_c.list, LV_SCROLLBAR_MODE_AUTO);

    memset(s_c.rows, 0, sizeof(s_c.rows));
    if (s_c.count == 0) {
        lv_obj_t *l = deck_label(s_c.list, g_font.mono_s, g_pal.dim,
                                 hal_storage_root() ? "No photos yet. SHOT in the live view saves one here."
                                                    : "No storage: photos need the SD card or internal flash.");
        lv_obj_set_width(l, LV_PCT(100));
    }
    for (int i = 0; i < s_c.count; i++) {
        lv_obj_t *r = deck_panel(s_c.list, DECK_CUT_DIAG, 6);
        lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(r, 6, 0);
        lv_obj_set_style_pad_ver(r, 3, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *row = deck_icon_text(r, ICON_IMAGE, s_c.names[i], g_font.mono_s, g_pal.text);
        (void)row;
        deck_panel_set_outline(r, g_pal.dim);
        s_c.rows[i] = r;
    }
    s_c.sel = 0;
    gallery_select(sel);
}

/* ---- One photo ----------------------------------------------------------- */

static void delete_confirmed(bool yes, void *user)
{
    int index = (int)(intptr_t)user;
    if (!yes || index < 0 || index >= s_c.count) return;
    photo_delete(s_c.names[index]);
    show_gallery(index > 0 ? index - 1 : 0);
}

static void delete_ask(int index)
{
    if (index < 0 || index >= s_c.count) return;
    char msg[80];
    snprintf(msg, sizeof(msg), "Delete %s?", s_c.names[index]);
    deck_modal_confirm_danger("DELETE PHOTO", msg, "DELETE", delete_confirmed, (void *)(intptr_t)index);
}

static void gallery_clicked_back(lv_event_t *e)
{
    (void)e;
    show_gallery(s_c.sel);
}

static void delete_clicked(lv_event_t *e)
{
    (void)e;
    delete_ask(s_c.sel);
}

static void show_photo(int index)
{
    if (index < 0 || index >= s_c.count) return;
    s_c.view = VIEW_PHOTO;
    s_c.live = false;
    s_c.sel  = index;
    lv_obj_t *root = page();

    lv_obj_t *frame = deck_box(root);
    lv_obj_set_size(frame, VIEW_W, VIEW_H);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    s_c.photo_img = lv_image_create(frame);
    lv_obj_center(s_c.photo_img);

    size_t len = 0;
    char *jpg  = photo_load(s_c.names[index], &len);
    uint16_t *pix;
    int w = 0, h = 0;
    char info[64] = "CANNOT READ";
    if (jpg && cam_jpeg_decode((const uint8_t *)jpg, len, VIEW_W * 2, VIEW_H * 2, &pix, &w, &h)) {
        /* Decoded at the smallest power-of-two step that still covers the
         * window, then scaled the rest of the way by LVGL. */
        static lv_image_dsc_t dsc;
        show_pixels(s_c.photo_img, &dsc, &s_c.photo_pix, pix, w, h);
        int32_t sx = VIEW_W * 256 / w, sy = VIEW_H * 256 / h;
        int32_t sc = sx < sy ? sx : sy;
        if (sc > 256) sc = 256;
        lv_image_set_scale(s_c.photo_img, sc);
        lv_obj_set_size(s_c.photo_img, w * sc / 256, h * sc / 256);
        lv_image_set_inner_align(s_c.photo_img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_center(s_c.photo_img);
        int fw, fh;
        if (cam_jpeg_info((const uint8_t *)jpg, len, &fw, &fh)) {
            snprintf(info, sizeof(info), "%dx%d  %lu KB", fw, fh, (unsigned long)(len / 1024));
        }
    }
    free(jpg);
    lv_obj_t *cap = deck_label(frame, g_font.mono_s, g_pal.accent, info);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_color(cap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(cap, 3, 0);

    lv_obj_t *bar = toolbar(root);
    tool_button(bar, "BACK", gallery_clicked_back, NULL);
    s_c.photo_name = deck_label(bar, g_font.mono_s, g_pal.dim, s_c.names[index]);
    lv_obj_set_flex_grow(s_c.photo_name, 1);
    lv_label_set_long_mode(s_c.photo_name, LV_LABEL_LONG_MODE_DOTS);
    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", index + 1, s_c.count);
    deck_label(bar, g_font.mono_s, g_pal.dim, pos);
    lv_obj_t *del = tool_button(bar, "DEL", delete_clicked, NULL);
    deck_panel_set_outline(del, g_pal.warn);
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    s_c.parent    = parent;
    s_c.screen    = NULL;
    s_c.alive     = true;
    s_c.busy      = false;
    s_c.shot_step = 0;
    s_c.shoot_req = false;
    s_c.retry     = NULL;
    s_c.fails     = 0;
    s_c.status_hold = 0;
    if (!hal_cfg_get_str("cam_host", s_c.host, sizeof(s_c.host)) || s_c.host[0] == '\0') {
        snprintf(s_c.host, sizeof(s_c.host), "192.168.4.1"); /* the unit's own access point */
    }
    s_c.size_idx = (int)hal_cfg_get_i32("cam_size", 0);
    if (s_c.size_idx < 0 || s_c.size_idx >= SIZE_COUNT) s_c.size_idx = 0;
    s_c.swap = hal_cfg_get_i32("cam_swap", 0) != 0;
    cam_jpeg_set_swap(s_c.swap);
    show_live();
    return true;
}

static void cam_exit(deck_app_t *self)
{
    (void)self;
    s_c.alive = false; /* replies still in flight are dropped */
    s_c.live  = false;
    if (s_c.retry) {
        lv_timer_delete(s_c.retry);
        s_c.retry = NULL;
    }
    if (s_c.img) lv_image_set_src(s_c.img, NULL);
    if (s_c.photo_img) lv_image_set_src(s_c.photo_img, NULL);
    lv_image_cache_drop(&s_c.dsc);
}

static void stop(deck_app_t *self)
{
    (void)self;
    free(s_c.pix);
    free(s_c.photo_pix);
    s_c.pix = s_c.photo_pix = NULL;
    s_c.screen = s_c.img = s_c.status = s_c.ap_btn = NULL;
    s_c.busy      = false;
    s_c.shot_step = 0;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_ALT | DECK_MOD_CTRL)) return false;
    bool back = k->code == HID_ESC || k->code == HID_DELETE || k->code == HID_BACKSPACE;
    switch (s_c.view) {
        case VIEW_LIVE:
            if (k->code == HID_ENTER || k->code == HID_SPACE) {
                shoot();
                return true;
            }
            if (k->code == HID_S) {
                size_cycle();
                return true;
            }
            if (k->code == HID_G) {
                show_gallery(0);
                return true;
            }
            if (k->code == HID_H) {
                host_prompt();
                return true;
            }
            if (k->code == HID_L) {
                led_toggle();
                return true;
            }
            if (k->code == HID_B) {
                swap_toggle();
                return true;
            }
            if (k->code == HID_A) {
                ap_hop();
                return true;
            }
            if (k->code == HID_W) {
                ap_pass_prompt();
                return true;
            }
            return false; /* back leaves the module */
        case VIEW_GALLERY:
            if (back) {
                show_live();
                return true;
            }
            if (k->code == HID_UP) {
                gallery_select(s_c.sel - 1);
                return true;
            }
            if (k->code == HID_DOWN) {
                gallery_select(s_c.sel + 1);
                return true;
            }
            if (k->code == HID_ENTER) {
                show_photo(s_c.sel);
                return true;
            }
            if (k->code == HID_D) {
                delete_ask(s_c.sel);
                return true;
            }
            return false;
        case VIEW_PHOTO:
            if (back) {
                show_gallery(s_c.sel);
                return true;
            }
            if (k->code == HID_LEFT || k->code == HID_UP) {
                if (s_c.sel > 0) show_photo(s_c.sel - 1);
                return true;
            }
            if (k->code == HID_RIGHT || k->code == HID_DOWN || k->code == HID_ENTER) {
                if (s_c.sel + 1 < s_c.count) show_photo(s_c.sel + 1);
                return true;
            }
            if (k->code == HID_D) {
                delete_ask(s_c.sel);
                return true;
            }
            return false;
    }
    return false;
}

static deck_app_t s_app = {
    .name     = "CAMERA",
    .tagline  = "UNITCAM S3",
    .icon     = ICON_CAMERA,
    .hotkey   = HID_LETTER('M'),
    .on_start = start,
    .on_exit  = cam_exit,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_camera(void) { return &s_app; }
