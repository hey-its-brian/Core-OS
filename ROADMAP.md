# Core//OS roadmap

## v0.1.0 (2026-10-28)
- [x] CoreS3 HAL on the Espressif BSP: display, touch, backlight, SD, settings
- [x] Faces Keyboard3 driver with HID translation (del = back, SYM+0 = Esc)
- [x] Faces Bottom3 side LEDs in the accent color (OFF / LOW / HIGH in SYSTEM)
- [x] Shell fitted to 320x240: status bar, 3 x 3 home grid, boot POST, modals
- [x] SYSTEM and CALC
- [x] First boot on the hardware: keyboard, touch, RTC, SD, side LEDs (2026-10-27)
- [ ] Wi-Fi join, battery readout and OTA check on the hardware
- [x] Release process (signed OTA images, GitHub releases)

## v0.2 (2026-10-28)
- [x] NOTES with the Obsidian vault sync (landed early, in v0.1.0)
- [x] WEATHER with its home tile widget (temperature and conditions on the tile)

## v0.3
- [x] NET TOOLS (SCAN, PING, LAN, SPEED), moved up from v0.4
- FILES (browser, portal)

## v0.4
- FILES, if it slips from v0.3

## v0.5
- TERMINAL (SSH, Ollama)

## v0.6
- RF WATCH with the RF Sentinel board

## Later
- Battery gauge calibration against the AXP2101 fuel gauge
- Keyboard3 firmware check (IAP) from SYSTEM
- CAMERA: the UnitCamS3 module (viewfinder, photos, gallery, Wi-Fi hop onto
  the unit's AP) is finished on the `camera` branch. Shelved 2026-10-28 because
  the Grove cable only powers the unit; a cable-only version needs custom
  firmware on the camera streaming JPEGs over UART. Its home tile is left open.
