/*
 * TERMINAL: scheduled for Core//OS v0.5. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_term.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> SSH ............ libssh2 over Wi-Fi, 53x16 cells\n"
                            "> OLLAMA ......... chat with a local model\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "TERMINAL",
    .tagline  = "SSH UPLINK",
    .icon     = ICON_CONSOLE,
    .eta      = "v0.5",
    .hotkey   = HID_LETTER('T'),
    .on_start = start,
};

deck_app_t *app_term(void) { return &s_app; }
