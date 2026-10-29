# Core//OS

Cyberdeck firmware for the **M5Stack CoreS3 Lite** on a **Faces Bottom3** with
the **Faces Keyboard3**. A port of [DECK//OS](https://github.com/hey-its-brian/tab5-cyberdeck)
(the Tab5 cyberdeck) to a 2 inch, 320x240 screen and a 35 key keyboard: the
same modules, the same look, fitted to a pocket deck.

Status: **v0.3.0 in progress**. The shell, home grid, SYSTEM, CALC, NOTES
(with Obsidian vault sync), WEATHER and NET TOOLS work; the other modules
show their manifests until they are ported. Updates arrive over the air from
this repository's releases.

## What runs

| Module    | State  | Notes |
|-----------|--------|-------|
| SYSTEM    | ready  | brightness, scanlines, boot POST, saver, side LEDs, accent, Wi-Fi join, timezone, clock, hardware readout, OTA updates |
| CALC      | ready  | expression evaluator, tape, hex/oct/bin, SCI and PROG keypads |
| NOTES     | ready  | markdown notes, folders, editor and preview, Obsidian vault sync (LiveSync over CouchDB) |
| WEATHER   | ready  | Open-Meteo, no key: NOW, 24H trace, 7 DAY strip; city search; offline cache; live home tile |
| FILES     | v0.3   | SD browser and the browser portal |
| (open)    |        | a free tile; the UnitCamS3 CAMERA module lives on the `camera` branch |
| NET TOOLS | ready  | Wi-Fi scanner with channel chart, ping / DNS / traceroute / port check, LAN sweep with mDNS names, speed test |
| TERMINAL  | v0.5   | SSH and Ollama chat |
| RF WATCH  | v0.6   | the RF Sentinel board |

## Hardware

- CoreS3 Lite: ESP32-S3, 16 MB flash, 8 MB PSRAM, ILI9342C 320x240 with
  FT6336U touch, AXP2101 PMIC, AW9523B expander, BM8563 RTC, AW88298 speaker,
  SD card. Brought up through Espressif's `m5stack_core_s3` BSP.
- Faces Bottom3: 600 mAh battery, ten SK6812 LEDs (five a side) on the M-Bus
  5 V rail, LED data on G5 with the base's LED IO switch on its "CoreS3" side.
- Faces Keyboard3: STM32 at I2C 0x08 on the system bus (G12 / G11), one byte
  per key, polled every 12 ms and translated to HID usages in `deck_hal`.

## Keys

The Keyboard3 has no Esc or Ctrl. On this deck:

| Key            | Does |
|----------------|------|
| del            | back / close (deletes inside text fields) |
| SYM+0          | Esc |
| ALT+1..9       | open a home tile |
| ALT+letter     | jump to a module (T N C W F R A S) |
| ALT+SPACE      | back to the deck from anywhere |
| ALT+0          | sleep the screen (or tap the clock) |
| ALT+H          | shortcut list |
| FN+K N M $     | up, left, down, right |

## Build

ESP-IDF 5.5. Dependencies come from the component registry on the first build.

```bash
. ~/esp/esp-idf-v5.5/export.sh
idf.py set-target esp32s3
idf.py build flash monitor
```

Desktop simulator (SDL2), with headless screenshots:

```bash
cmake -S sim -B sim/build && cmake --build sim/build -j
sim/build/deck_sim
sim/build/deck_sim --headless --no-boot --shot 1500:shots/home.ppm
```

## Layout

- `components/deck_hal/hal_cores3.c`: the board: display and touch through the
  BSP, Keyboard3 polling and translation, AXP2101 battery, BM8563 clock, SD,
  encrypted settings (NVS with an HMAC eFuse key), the side LEDs.
- `components/deck_core`: shell, status bar, 3 x 3 launcher, boot POST,
  modals, screensaver, updater.
- `components/deck_ui`: palette, fonts (sized for 320x240), widget kit.
- `components/deck_net`, `deck_ota`, `deck_sync`, `deck_md`,
  `deck_portal`, `deck_term`: shared engines carried over from DECK//OS.
- `apps/app_*`: one module per folder, registered in `main/main.c` and
  `sim/main.c`.

Releases are signed app images (no Secure Boot, no flash encryption) with
rollback: `tools/release.sh`.
