#include "deck_shell.h"

#include "deck_boot.h"
#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_launcher.h"
#include "deck_modal.h"
#include "deck_saver.h"
#include "deck_statusbar.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TRANSITION_MS 260

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static deck_app_t *s_current;    /* running app, NULL on the home or a group screen */
static deck_group_t *s_group;    /* group screen shown, or the group the running app sits in */
static deck_app_t *s_next;       /* what to show at the midpoint of a transition */
static deck_group_t *s_next_group;
static bool s_busy;              /* a transition is running */

/* ---- Building blocks ----------------------------------------------------- */

static void build_root(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, g_pal.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_root = deck_box(scr);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    deck_statusbar_create(s_root);

    s_content = deck_box(s_root);
    lv_obj_set_width(s_content, LV_PCT(100));
    lv_obj_set_flex_grow(s_content, 1);
}

/* Delete whatever is on screen, then stop the app. The order matters:
 * removing focused widgets fires DEFOCUSED events into the app's handlers,
 * which must still find the app's state intact. */
static void teardown(void)
{
    deck_app_t *app = s_current;
    s_current       = NULL;
    if (app && app->on_exit) app->on_exit(app);
    deck_modal_discard();
    lv_group_remove_all_objs(deck_input_group());
    lv_obj_clean(s_content);
    deck_input_reset();
    if (app && app->on_stop) app->on_stop(app);
}

/* The home screen (group NULL) or a group screen. */
static void show_launcher_now(deck_group_t *group)
{
    teardown();
    s_group = group;
    deck_launcher_create(s_content, group);
    deck_statusbar_set_path(group, NULL);
}

static void start_app_now(deck_app_t *app)
{
    teardown();
    s_group = deck_app_group(app);
    if (!app->on_start(app, s_content)) {
        LV_LOG_ERROR("module %s failed to start", app->name);
        lv_group_remove_all_objs(deck_input_group());
        lv_obj_clean(s_content);
        deck_launcher_create(s_content, s_group);
        deck_statusbar_set_path(s_group, NULL);
        return;
    }
    s_current = app;
    deck_statusbar_set_path(s_group, app);
}

/* ---- Transitions --------------------------------------------------------- */

static void transition_mid(void *u)
{
    (void)u;
    if (s_next) {
        start_app_now(s_next);
    } else {
        show_launcher_now(s_next_group);
    }
}

static void transition_done(void *u)
{
    (void)u;
    s_busy = false;
}

static void go(deck_app_t *app, deck_group_t *group)
{
    s_busy       = true;
    s_next       = app;
    s_next_group = group;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

void deck_shell_launch(deck_app_t *app)
{
    if (app == NULL || s_busy || app == s_current) return;
    go(app, NULL);
}

void deck_shell_open_group(deck_group_t *group)
{
    if (group == NULL || s_busy || (s_current == NULL && s_group == group)) return;
    go(NULL, group);
}

void deck_shell_home(void)
{
    if (s_busy || (s_current == NULL && s_group == NULL)) return;
    go(NULL, NULL);
}

void deck_shell_back(void)
{
    if (s_busy) return;
    if (s_current) {
        go(NULL, s_group); /* the app's group, or home */
    } else if (s_group) {
        go(NULL, NULL);
    }
}

deck_app_t *deck_shell_current(void) { return s_current; }
deck_group_t *deck_shell_group(void) { return s_group; }

/* ---- Sleep and keyboard light -------------------------------------------- */

static lv_obj_t *s_sleep;

static void wake_async(void *u)
{
    (void)u;
    if (s_sleep) {
        lv_obj_delete(s_sleep);
        s_sleep = NULL;
    }
    hal_display_power(true);
}

static void sleep_pressed(lv_event_t *e)
{
    (void)e;
    lv_async_call(wake_async, NULL); /* not from inside the overlay's own event */
}

void deck_shell_sleep(void)
{
    if (s_sleep) return;
    /* A black, clickable layer above everything (modals included) so the
     * waking touch lands here instead of on a control. */
    s_sleep = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sleep);
    lv_obj_set_size(s_sleep, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sleep, sleep_pressed, LV_EVENT_PRESSED, NULL);
    hal_display_power(false);
}

bool deck_shell_sleeping(void) { return s_sleep != NULL; }

static const uint8_t s_kbd_levels[3] = {0, 3, 20}; /* OFF, LOW, HIGH (20 = keyboard default) */

void deck_shell_apply_kbd_light(void)
{
    int level = (int)hal_cfg_get_i32("kbd_led", 1);
    if (level < 0 || level > 2) level = 1;
    bool theme   = true; /* the side LEDs always take the accent */
    uint32_t rgb = lv_color_to_u32(g_pal.accent) & 0xFFFFFF;
    hal_kbd_light(s_kbd_levels[level], theme, rgb);
}

/* ---- Shortcut list (Alt+H) ---------------------------------------------- */

void deck_shell_shortcuts(void)
{
    static const char *const cols[] = {
        "ANYWHERE\n"
        "  ALT+1..9    open a tile\n"
        "  ALT+letter  jump to a module\n"
        "  ALT+SPACE   back to the deck\n"
        "  ALT+0       sleep (or tap the clock)\n"
        "  ALT+H       this list\n"
        "  DEL         back / close\n"
        "  SYM+0       Esc\n"
        "  FN+K N M $  arrows\n"
        "\n"
        "HOME\n"
        "  arrows + ENTER, or 1..9\n"
        "\n"
        "NOTES\n"
        "  N new  R rename  D delete  S sync\n"
        "  ALT+V edit / view  ALT+E save\n"
        "\n"
        "CALC\n"
        "  ENTER evaluate  UP/DOWN recall\n"
        "  ALT+D deg/rad   ALT+K keypad\n"
        "  ALT+L clear tape\n"
        "\n"
        "CAMERA\n"
        "  ENTER shoot  S size  G gallery\n"
        "  A cam Wi-Fi / home  H host  L led",
    };
    deck_modal_info("SHORTCUTS", cols, 1);
}

/* Deferred: rebuild is usually requested from an event handler of a widget
 * that the rebuild deletes. */
static void rebuild_async(void *u)
{
    (void)u;
    deck_app_t *app     = s_current;
    deck_group_t *group = s_group;
    teardown();
    deck_statusbar_destroy();
    lv_obj_delete(s_root);
    deck_input_reset();
    deck_shell_apply_kbd_light(); /* accent may have changed */
    build_root();
    if (app) {
        start_app_now(app);
    } else {
        show_launcher_now(group);
    }
}

void deck_shell_rebuild(void) { lv_async_call(rebuild_async, NULL); }

/* ---- Keys ---------------------------------------------------------------- */

/* The Keyboard3 has no Esc key (Sym+0 sends one), so its "del" key (Backspace,
 * Sym+del is Delete) doubles as back, except while a text field has focus,
 * where it keeps deleting. Modules that use it themselves consume it first. */
static bool back_key(const deck_key_t *k)
{
    if (k->code == HID_ESC) return true;
    if ((k->code != HID_DELETE && k->code != HID_BACKSPACE) || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) {
        return false;
    }
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    return f == NULL || !lv_obj_check_type(f, &lv_textarea_class);
}

bool deck_shell_key(const deck_key_t *k)
{
    deck_saver_poke(); /* every key is activity, even ones the shell consumes */
    if (deck_saver_active()) {
        deck_saver_wake(); /* the waking key does nothing else */
        return true;
    }
    if (s_sleep) {
        wake_async(NULL); /* the waking key does nothing else */
        return true;
    }
    if (deck_boot_active()) {
        deck_boot_skip();
        return true;
    }
    if (s_busy) return true; /* swallow keys mid-transition */

    if (deck_modal_active()) {
        if (back_key(k)) { /* Esc, or del outside a text field */
            deck_modal_cancel();
            return true;
        }
        if (k->code == HID_ENTER) {
            deck_modal_enter();
            return true;
        }
        return (k->mods & DECK_MOD_ALT) != 0; /* no module switching under a dialog */
    }

    if (k->mods & DECK_MOD_ALT) {
        if (k->code == HID_ESC || k->code == HID_SPACE) { /* Alt+Space: the Keyboard3 has no Esc */
            deck_shell_home();
            return true;
        }
        if (k->code == HID_0) {
            deck_shell_sleep();
            return true;
        }
        if (k->code == HID_H) {
            deck_shell_shortcuts();
            return true;
        }
        if (k->code >= HID_1 && k->code <= HID_9) {
            const deck_tile_t *t = deck_home_tile((size_t)(k->code - HID_1));
            if (t && t->group) {
                deck_shell_open_group(t->group);
            } else if (t) {
                deck_shell_launch(t->app);
            }
            return true;
        }
        deck_app_t *app = deck_app_by_hotkey(k->code);
        if (app) {
            deck_shell_launch(app);
            return true;
        }
    }

    if (s_current == NULL) {
        if (deck_launcher_key(k)) return true;
        if (back_key(k) && s_group) {
            deck_shell_back();
            return true;
        }
        return false;
    }
    if (s_current->on_key && s_current->on_key(s_current, k)) {
        return true;
    }
    if (back_key(k)) {
        deck_shell_back();
        return true;
    }
    return false;
}

/* ---- Start --------------------------------------------------------------- */

void deck_shell_start(bool boot_anim)
{
    deck_grid_bg(lv_screen_active());
    build_root();
    show_launcher_now(NULL);
    deck_fx_scanlines(hal_cfg_get_i32("scan", 1) != 0);
    deck_shell_apply_kbd_light();
    if (boot_anim) deck_boot_run();
}
