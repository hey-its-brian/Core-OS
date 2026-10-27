/*
 * WEATHER: scheduled for Core//OS v0.2. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_weather.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> NOW ............ conditions for a saved city\n"
                            "> 24H ............ temperature and rain trace\n"
                            "> 7 DAY .......... outlook strip\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "WEATHER",
    .tagline  = "OPEN-METEO",
    .icon     = ICON_WEATHER,
    .eta      = "v0.2",
    .hotkey   = HID_LETTER('W'),
    .on_start = start,
};

deck_app_t *app_weather(void) { return &s_app; }
