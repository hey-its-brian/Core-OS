/*
 * NOTES: scheduled for Core//OS v0.2. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_notes.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> LOCAL NOTES .... markdown on the SD card\n"
                            "> VAULT .......... Obsidian LiveSync over CouchDB\n"
                            "> PREVIEW ........ rendered markdown\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "NOTES",
    .tagline  = "MARKDOWN + VAULT",
    .icon     = ICON_MARKDOWN,
    .eta      = "v0.2",
    .hotkey   = HID_LETTER('N'),
    .on_start = start,
};

deck_app_t *app_notes(void) { return &s_app; }
