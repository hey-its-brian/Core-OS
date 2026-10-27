/*
 * FILES: scheduled for Core//OS v0.3. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_files.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> BROWSE ......... the SD card and internal storage\n"
                            "> PORTAL ......... browser upload / download over Wi-Fi\n"
                            "> PREVIEW ........ text, markdown, images\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "FILES",
    .tagline  = "SD + PORTAL",
    .icon     = ICON_FOLDER,
    .eta      = "v0.3",
    .hotkey   = HID_LETTER('F'),
    .on_start = start,
};

deck_app_t *app_files(void) { return &s_app; }
