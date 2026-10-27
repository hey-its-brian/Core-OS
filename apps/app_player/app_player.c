/*
 * PLAYER: scheduled for Core//OS v0.3. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_player.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> QUEUE .......... one folder of MP3s from the SD card\n"
                            "> SPEAKER ........ AW88298 on the CoreS3\n"
                            "> VISUALIZER ..... 16 band spectrum\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "PLAYER",
    .tagline  = "MP3 DECK",
    .icon     = ICON_MUSIC,
    .eta      = "v0.3",
    .hotkey   = HID_LETTER('M'),
    .on_start = start,
};

deck_app_t *app_player(void) { return &s_app; }
