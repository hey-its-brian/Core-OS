# Third-party components

| Component | Source | License | Notes |
|---|---|---|---|
| `components/md4c` | [mity/md4c](https://github.com/mity/md4c) 0.5.2 | MIT | Markdown parser, unmodified. |
| esp_codec_dev | [espressif/esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) 1.5 via the component registry | Apache 2.0 | AW88298 amplifier driver and I2S data path. |
| `components/libvterm` | [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3 | MIT | Terminal emulation, unmodified. |
| JetBrains Mono (subset) | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/jetbrainsmono) | SIL OFL 1.1 | `assets/fonts/OFL-JetBrainsMono.txt`; subset by `tools/subset_term_font.py` |
| LVGL 9.5 | [lvgl/lvgl](https://github.com/lvgl/lvgl) via the component registry | MIT | |
| Orbitron | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/orbitron) | SIL OFL 1.1 | `assets/fonts/OFL-Orbitron.txt` |
| Share Tech Mono | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/sharetechmono) | SIL OFL 1.1 | `assets/fonts/OFL-ShareTechMono.txt` |
| Material Design Icons (subset) | [Templarian/MaterialDesign](https://github.com/Templarian/MaterialDesign) | Pictogrammers Free License (icons Apache 2.0) | `assets/fonts/MDI_LICENSE`; subset built by `tools/subset_icons.sh` |

Keyboard protocol reference: [Faces Keyboard3 docs](https://docs.m5stack.com/en/faces/Faces_Keyboard3)
and the [M5Faces](https://github.com/m5stack/M5Faces) driver.

## Core//OS additions

- Espressif `m5stack_core_s3` board support package (Apache-2.0): CoreS3 display,
  touch, PMIC rails, IO expander, SD card and audio bring-up.
- Espressif `led_strip` (Apache-2.0): the Faces Bottom3 SK6812 side LEDs over RMT.
- M5Stack `M5Faces` (MIT) was the reference for the Faces Keyboard3 register
  map and key codes; the driver in `components/deck_hal/hal_cores3.c` is a
  rewrite in C, no code was copied.
