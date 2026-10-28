#include "cam_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_swap;

void cam_jpeg_set_swap(bool swap) { s_swap = swap; }

/* Scale steps: 0 = 1/1, 1 = 1/2, 2 = 1/4, 3 = 1/8. */
static int fit_shift(int w, int h, int max_w, int max_h)
{
    int s = 0;
    while (s < 3 && ((max_w > 0 && (w >> s) > max_w) || (max_h > 0 && (h >> s) > max_h))) s++;
    return s;
}

#ifdef ESP_PLATFORM

#include "esp_heap_caps.h"
#include "jpeg_decoder.h"

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

bool cam_jpeg_info(const uint8_t *jpg, size_t n, int *w, int *h)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpg,
        .indata_size = (uint32_t)n,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t o;
    if (esp_jpeg_get_image_info(&cfg, &o) != ESP_OK) return false;
    *w = o.width;
    *h = o.height;
    return true;
}

bool cam_jpeg_decode(const uint8_t *jpg, size_t n, int max_w, int max_h, uint16_t **out, int *w, int *h)
{
    int fw, fh;
    if (!cam_jpeg_info(jpg, n, &fw, &fh)) return false;
    int shift = fit_shift(fw, fh, max_w, max_h);
    int ow = fw >> shift, oh = fh >> shift;
    if (ow <= 0 || oh <= 0) return false;
    size_t bytes = (size_t)ow * (size_t)oh * 2;
    uint16_t *buf = big_alloc(bytes);
    if (buf == NULL) return false;
    esp_jpeg_image_cfg_t cfg = {
        .indata                = (uint8_t *)jpg,
        .indata_size           = (uint32_t)n,
        .outbuf                = (uint8_t *)buf,
        .outbuf_size           = (uint32_t)bytes,
        .out_format            = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale             = (esp_jpeg_image_scale_t)shift,
        .flags.swap_color_bytes = s_swap,
    };
    esp_jpeg_image_output_t o;
    if (esp_jpeg_decode(&cfg, &o) != ESP_OK) {
        free(buf);
        return false;
    }
    *out = buf;
    *w   = o.width;
    *h   = o.height;
    return true;
}

#else /* simulator: sips does the decoding, we read back a BMP */

#include <unistd.h>

static bool run(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (p == NULL) return false;
    char line[256];
    while (fgets(line, sizeof(line), p)) {}
    return pclose(p) == 0;
}

static const char *tmpdir(void)
{
    const char *t = getenv("TMPDIR");
    return t && t[0] ? t : "/tmp";
}

static bool dump(const uint8_t *jpg, size_t n, char *path, size_t plen)
{
    snprintf(path, plen, "%s/cam_%ld.jpg", tmpdir(), (long)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    bool ok = fwrite(jpg, 1, n, f) == n;
    fclose(f);
    return ok;
}

bool cam_jpeg_info(const uint8_t *jpg, size_t n, int *w, int *h)
{
    char path[256];
    if (!dump(jpg, n, path, sizeof(path))) {
        fprintf(stderr, "cam_jpeg: cannot write %s\n", path);
        return false;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sips -g pixelWidth -g pixelHeight '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (p == NULL) {
        unlink(path);
        return false;
    }
    char line[128];
    *w = *h = 0;
    while (fgets(line, sizeof(line), p)) {
        int v;
        if (sscanf(line, " pixelWidth: %d", &v) == 1) *w = v;
        if (sscanf(line, " pixelHeight: %d", &v) == 1) *h = v;
    }
    pclose(p);
    unlink(path); /* only once sips is done with it */
    return *w > 0 && *h > 0;
}

bool cam_jpeg_decode(const uint8_t *jpg, size_t n, int max_w, int max_h, uint16_t **out, int *w, int *h)
{
    int fw, fh;
    if (!cam_jpeg_info(jpg, n, &fw, &fh)) return false;
    int shift = fit_shift(fw, fh, max_w, max_h);
    int ow = fw >> shift, oh = fh >> shift;
    char path[256], bmp[256], cmd[768];
    if (!dump(jpg, n, path, sizeof(path))) return false;
    snprintf(bmp, sizeof(bmp), "%s/cam_%ld.bmp", tmpdir(), (long)getpid());
    snprintf(cmd, sizeof(cmd), "sips -s format bmp -z %d %d '%s' --out '%s' >/dev/null 2>&1", oh, ow, path, bmp);
    bool ok = run(cmd);
    unlink(path);
    if (!ok) {
        fprintf(stderr, "cam_jpeg: sips failed: %s\n", cmd);
        return false;
    }
    FILE *f = fopen(bmp, "rb");
    if (f == NULL) {
        fprintf(stderr, "cam_jpeg: no bmp %s\n", bmp);
        return false;
    }
    uint8_t hdr[54];
    ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    uint32_t off = hdr[10] | hdr[11] << 8 | hdr[12] << 16 | (uint32_t)hdr[13] << 24;
    int bw        = (int)(hdr[18] | hdr[19] << 8 | hdr[20] << 16 | (uint32_t)hdr[21] << 24);
    int bh        = (int)(hdr[22] | hdr[23] << 8 | hdr[24] << 16 | (uint32_t)hdr[25] << 24);
    int bpp       = hdr[28] | hdr[29] << 8;
    bool flip     = bh > 0;
    if (bh < 0) bh = -bh;
    ok = ok && (bpp == 24 || bpp == 32) && bw > 0 && bh > 0;
    uint16_t *buf = ok ? malloc((size_t)bw * (size_t)bh * 2) : NULL;
    size_t row    = ((size_t)bpp * (size_t)bw + 31) / 32 * 4;
    uint8_t *line = buf ? malloc(row) : NULL;
    if (buf && line) {
        fseek(f, (long)off, SEEK_SET);
        for (int y = 0; y < bh && ok; y++) {
            ok = fread(line, 1, row, f) == row;
            int dy = flip ? bh - 1 - y : y;
            for (int x = 0; x < bw; x++) {
                const uint8_t *px = line + (size_t)x * (size_t)(bpp / 8);
                uint16_t v = (uint16_t)((px[2] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[0] >> 3);
                if (s_swap) v = (uint16_t)(v << 8 | v >> 8);
                buf[(size_t)dy * (size_t)bw + (size_t)x] = v;
            }
        }
    } else {
        ok = false;
    }
    free(line);
    fclose(f);
    unlink(bmp);
    if (!ok) {
        fprintf(stderr, "cam_jpeg: bmp parse failed bpp %d %dx%d\n", bpp, bw, bh);
        free(buf);
        return false;
    }
    *out = buf;
    *w   = bw;
    *h   = bh;
    return true;
}

#endif
