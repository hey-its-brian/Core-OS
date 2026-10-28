/*
 * JPEG to RGB565 for the CAMERA module. On the device this is the ROM
 * TJpgDec through espressif/esp_jpeg (scales by 1, 1/2, 1/4 or 1/8 while it
 * decodes, so a 1600x1200 photo never has to exist in RAM). The simulator
 * goes through macOS sips instead.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel size of a JPEG without decoding it. */
bool cam_jpeg_info(const uint8_t *jpg, size_t n, int *w, int *h);

/* Decode at the largest power-of-two scale whose result fits max_w x max_h
 * (0 = no limit). *out is malloc'd (PSRAM when the device has it); the
 * caller frees it. Pixels are LVGL-native RGB565. */
bool cam_jpeg_decode(const uint8_t *jpg, size_t n, int max_w, int max_h, uint16_t **out, int *w, int *h);

/* Some decoder builds emit the two colour bytes the other way round. The
 * CAMERA module exposes this as a toggle so the user can fix a wrong-looking
 * picture on the spot. */
void cam_jpeg_set_swap(bool swap);

#ifdef __cplusplus
}
#endif
