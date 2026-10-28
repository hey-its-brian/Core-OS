/*
 * Core//OS entry point: bring up the hardware, then hand the screen to the
 * shell. Everything after that runs on the LVGL task.
 */
#include "deck_hal.h"
#include "deck_home.h"
#include "deck_icons.h"
#include "deck_input.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_shell.h"
#include "deck_sync.h"
#include "deck_theme.h"

#include "app_calc.h"
#include "app_files.h"
#include "app_net.h"
#include "app_notes.h"
#include "app_rf.h"
#include "app_sys.h"
#include "app_term.h"
#include "app_weather.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Parsed JSON (weather, releases, a vault sync's pages) lives in PSRAM so
 * internal RAM stays free for the Wi-Fi, TLS and DMA buffers that need it. */
static void *json_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

void app_main(void)
{
    if (!deck_hal_init()) {
        ESP_LOGE("main", "display bring-up failed, halting");
        return;
    }

    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    net_init();             /* brings up the radio in the background */
    ota_init(DECK_VERSION); /* arms the rollback countdown on a fresh OTA build */


    hal_lvgl_lock(0);
    deck_theme_init();
    deck_input_init();

    /* Every module, flat: Alt+letter shortcuts and lookups by name. */
    deck_app_register(app_term());
    deck_app_register(app_notes());
    deck_app_register(app_calc());
    deck_app_register(app_weather());
    deck_app_register(app_files());
    deck_app_register(app_net());
    deck_app_register(app_rf());
    deck_app_register(app_sys());
    deck_sync_set_backend(&g_sync_livesync); /* vault sync for NOTES */

    /* Home tiles 1..9 (Alt+1..9), one square each. */
    deck_home_add_app(app_term());
    deck_home_add_app(app_notes());
    deck_home_add_app(app_calc());
    deck_home_add_app(app_weather());
    deck_home_add_app(app_files());
    deck_home_add_blank(); /* open slot */
    deck_home_add_app(app_net());
    deck_home_add_app(app_rf());
    deck_home_add_app(app_sys());

    deck_shell_start(hal_cfg_get_i32("boot", 1) != 0);
    hal_lvgl_unlock();
}
