/*
 * NET TOOLS: scheduled for Core//OS v0.4. Shows its manifest until the DECK//OS
 * module is fitted to this screen.
 */
#include "app_net.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> SCAN ........... every access point, per BSSID\n"
                            "> PING ........... ping, DNS, traceroute, port check\n"
                            "> LAN ............ sweep plus mDNS and ARP\n"
                            "> SPEED .......... download / upload probe\n"
                            "\n"
                            "PORT OF THE DECK//OS MODULE, FIT TO 320x240.");
}

static deck_app_t s_app = {
    .name     = "NET TOOLS",
    .tagline  = "WI-FI + LAN",
    .icon     = ICON_LAN,
    .eta      = "v0.4",
    .hotkey   = HID_LETTER('R'),
    .on_start = start,
};

deck_app_t *app_net(void) { return &s_app; }
