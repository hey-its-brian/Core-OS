/*
 * deck_hal for the M5Stack CoreS3 Lite + Faces Keyboard3 (Core//OS).
 *
 * Board bring-up goes through Espressif's m5stack_core_s3 BSP: AXP2101
 * rails, AW9523B expander, ILI9342C over SPI, FT6336U touch and the SD card
 * on the same SPI bus. The keyboard is the Faces Keyboard3 on the Faces
 * Bottom3 (an STM32 at I2C 0x08 on the system bus), polled here and
 * translated to HID usages so the rest of the deck never sees the
 * difference.
 */
#include "deck_hal.h"
#include "deck_tz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_err.h" /* bsp/display.h forgets it */
#include "bsp/display.h"
#include "bsp/m5stack_core_s3.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_io_expander.h"
#include "led_strip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

static const char *TAG = "hal";

static lv_display_t *s_disp;
static QueueHandle_t s_key_q;
static volatile bool s_kbd_present;
static uint8_t s_kbd_fw;
static bool s_rtc_ok;
static bool s_axp_ok;
static bool s_sd_ok;
static sdmmc_card_t *s_card;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static bool s_nvs_encrypted;
static char s_chip_name[32];

/* ---- Display ------------------------------------------------------------- */

static bool display_init(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags         = {.buff_dma = true, .buff_spiram = false, .sw_rotate = false},
    };
    cfg.lvgl_port_cfg.task_priority     = 5;
    cfg.lvgl_port_cfg.task_stack        = 16 * 1024;
    cfg.lvgl_port_cfg.task_affinity     = 1;
    cfg.lvgl_port_cfg.task_max_sleep_ms = 500;
    cfg.lvgl_port_cfg.timer_period_ms   = 5;
    s_disp                              = bsp_display_start_with_config(&cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "display bring-up failed");
        return false;
    }
    return true;
}

/* The Espressif FT5x06 driver raises the touch threshold to 70 and lets the
 * controller drop into its slow "monitor" scan after 2 s idle, so light or
 * quick taps go missing when polled. M5's own driver leaves the chip at its
 * factory tuning and only picks polling mode. Do the same: reset it through
 * the AW9523 and set just the mode registers. */
#define TOUCH_ADDR 0x38
static void touch_tune(void)
{
    bsp_feature_enable(BSP_FEATURE_TOUCH, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(300)); /* FT6336U start-up */
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDR, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &dev) != ESP_OK) return;
    static const uint8_t regs[][2] = {
        {0x00, 0x00}, /* working mode */
        {0xA4, 0x00}, /* INT polling mode */
        {0x86, 0x00}, /* stay in active scan, never drop to monitor mode */
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        i2c_master_transmit(dev, regs[i], 2, 50);
    }
    i2c_master_bus_rm_device(dev);

    /* A finger on a 2 inch panel wobbles; 10 px is less than 2 mm here, and
     * a tap that moves that far turned into a scroll. */
    lv_indev_t *tp = bsp_display_get_input_dev();
    if (tp && lvgl_port_lock(100)) {
        lv_indev_set_scroll_limit(tp, 16);
        lvgl_port_unlock();
    }
}

static uint8_t s_backlight = 80;
static bool s_display_on   = true;

void hal_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 3) percent = 3; /* never fully dark: the user could not find the slider */
    s_backlight = percent;
    if (s_display_on) bsp_display_brightness_set(percent);
}

static void kbd_light_apply(void);
static void rgb_apply(void);

void hal_display_power(bool on)
{
    s_display_on = on;
    bsp_display_brightness_set(on ? s_backlight : 0);
    if (s_kbd_present) kbd_light_apply();
    rgb_apply(); /* side LEDs follow the screen */
}

bool hal_lvgl_lock(uint32_t timeout_ms) { return bsp_display_lock(timeout_ms); }
void hal_lvgl_unlock(void) { bsp_display_unlock(); }

/* ---- Keyboard: Faces Keyboard3 ------------------------------------------- */

#define KBD_ADDR 0x08
#define KBD_REG_KEY 0x00
#define KBD_REG_MODEL 0xD0
#define KBD_REG_LED 0xF1
#define KBD_REG_FW 0xFE
#define KBD_MODEL_KEYBOARD3 0x02
#define KBD_POLL_MS 12

/* HID usages, the few this file needs (the rest of the deck has deck_input.h). */
#define HID_A 0x04
#define HID_1 0x1E
#define HID_0 0x27
#define HID_ENTER 0x28
#define HID_ESC 0x29
#define HID_BACKSPACE 0x2A
#define HID_TAB 0x2B
#define HID_SPACE 0x2C
#define HID_INSERT 0x49
#define HID_HOME 0x4A
#define HID_PGUP 0x4B
#define HID_DELETE 0x4C
#define HID_END 0x4D
#define HID_PGDN 0x4E
#define HID_RIGHT 0x4F
#define HID_LEFT 0x50
#define HID_DOWN 0x51
#define HID_UP 0x52
#define MOD_SHIFT 0x02
#define MOD_ALT 0x04

static i2c_master_dev_handle_t s_kbd;

static bool kbd_read(uint8_t reg, uint8_t *v, size_t n)
{
    return s_kbd && i2c_master_transmit_receive(s_kbd, &reg, 1, v, n, 50) == ESP_OK;
}

static bool kbd_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return s_kbd && i2c_master_transmit(s_kbd, b, 2, 50) == ESP_OK;
}

/* The firmware reports one byte: ASCII for the printable keys, control
 * codes for del / Enter / Tab, SYM+0 for Esc, and codes above 0x7F for
 * FN and ALT chords. Fold them into HID usage + modifiers (US layout). */
static bool kbd_translate(uint8_t raw, uint8_t *code, uint8_t *mods)
{
    static const char plain[] = "1234567890\0\0\0\0 -=[]\\\0;'`,./"; /* HID 0x1E.. */
    static const char shift[] = "!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
    static const char alt_row1[] = "QWERTYUIOPASDFGHJKL"; /* 144..162 */
    static const char alt_row2[] = "ZXCVBNM";             /* 165..171 */
    *mods = 0;
    switch (raw) {
        case 0x08: *code = HID_BACKSPACE; return true;
        case 0x7F: *code = HID_DELETE; return true;
        case 0x0D: *code = HID_ENTER; return true;
        case 0x09: *code = HID_TAB; return true;
        case 0x1B: *code = HID_ESC; return true;
        case 183: *code = HID_UP; return true;
        case 184: *code = HID_INSERT; return true;
        case 186: *code = HID_TAB; return true;
        case 187: *code = HID_HOME; return true;
        case 188: *code = HID_END; return true;
        case 189: *code = HID_PGUP; return true;
        case 190: *code = HID_PGDN; return true;
        case 191: *code = HID_LEFT; return true;
        case 192: *code = HID_DOWN; return true;
        case 193: *code = HID_RIGHT; return true;
        case 163: *code = HID_DELETE; *mods = MOD_ALT; return true;
        case 175: *code = HID_0; *mods = MOD_ALT; return true;
        case 176: *code = HID_SPACE; *mods = MOD_ALT; return true;
        default: break;
    }
    if (raw >= 144 && raw <= 162) {
        *code = (uint8_t)(HID_A + alt_row1[raw - 144] - 'A');
        *mods = MOD_ALT;
        return true;
    }
    if (raw >= 165 && raw <= 171) {
        *code = (uint8_t)(HID_A + alt_row2[raw - 165] - 'A');
        *mods = MOD_ALT;
        return true;
    }
    if (raw >= 'a' && raw <= 'z') {
        *code = (uint8_t)(HID_A + raw - 'a');
        return true;
    }
    if (raw >= 'A' && raw <= 'Z') {
        *code = (uint8_t)(HID_A + raw - 'A');
        *mods = MOD_SHIFT;
        return true;
    }
    for (int i = 0; i < (int)sizeof(plain) - 1; i++) {
        if (plain[i] && plain[i] == (char)raw) {
            *code = (uint8_t)(HID_1 + i);
            return true;
        }
        if (shift[i] && shift[i] == (char)raw) {
            *code = (uint8_t)(HID_1 + i);
            *mods = MOD_SHIFT;
            return true;
        }
    }
    return false; /* modifier self-codes (0x80..0x84), 0xFF "no mapping", unknown */
}

static bool kbd_try_attach(void)
{
    if (s_kbd == NULL) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = KBD_ADDR,
            .scl_speed_hz    = 100000,
        };
        if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &s_kbd) != ESP_OK) return false;
    }
    uint8_t model = 0;
    /* A missing keyboard answers with a NACK, which the driver logs as an
     * error; mute that while probing. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    bool ok = kbd_read(KBD_REG_MODEL, &model, 1);
    esp_log_level_set("i2c.master", ESP_LOG_WARN);
    if (!ok || model != KBD_MODEL_KEYBOARD3) return false;
    uint8_t fw = 0;
    s_kbd_fw   = kbd_read(KBD_REG_FW, &fw, 1) ? fw : 0;
    uint8_t drain;
    kbd_read(KBD_REG_KEY, &drain, 1);
    return true;
}

static volatile uint8_t s_kbd_light = 20;

static void kbd_light_apply(void)
{
    /* 0x00 lets the firmware drive the two LEDs as modifier indicators;
     * 0x80 is manual mode with both off. */
    kbd_write(KBD_REG_LED, s_kbd_light && s_display_on ? 0x00 : 0x80);
}

/* ---- Faces Bottom3 side LEDs ----------------------------------------------
 * Ten SK6812s, five a side, behind the Core on the Bottom3. Their data line
 * is the base's LED IO switch, which picks one of two M-Bus positions named
 * after the classic Core: "Core" is the old G15 position (I2S_OUT, pin 23),
 * which on the CoreS3 is G13, the speaker's I2S data; "CoreS3" is the old
 * G25 position (pin 8), which on the CoreS3 is G5. The switch goes on the
 * CoreS3 side and the data runs out of GPIO 5. They run from the M-Bus 5 V
 * rail, which the CoreS3 keeps off until asked: BOOST_EN (AW9523 P1_7) and
 * BUS_OUT_EN (P0_1), the latter pulsed up first so the inrush does not
 * drop the PMIC out on a weak USB supply (M5Unified does the same). */

#define RGB_GPIO 5
#define RGB_COUNT 10
#define EXP_BUS_OUT_EN IO_EXPANDER_PIN_NUM_1
#define EXP_BOOST_EN IO_EXPANDER_PIN_NUM_15

static led_strip_handle_t s_strip;
static bool s_bus_on;
static uint32_t s_rgb      = 0x00F0FF;
static uint8_t s_rgb_level = 0; /* 0..255 */

static void bus_5v(bool on)
{
    if (on == s_bus_on) return;
    esp_io_expander_handle_t io = bsp_io_expander_init();
    if (io == NULL) return;
    esp_io_expander_set_dir(io, EXP_BUS_OUT_EN | EXP_BOOST_EN, IO_EXPANDER_OUTPUT);
    if (on) {
        esp_io_expander_set_level(io, EXP_BOOST_EN, 1);
        for (int i = 0; i < 8; i++) { /* precharge pulses of growing on-time */
            esp_io_expander_set_level(io, EXP_BUS_OUT_EN, 1);
            esp_rom_delay_us((uint32_t)i * 16);
            esp_io_expander_set_level(io, EXP_BUS_OUT_EN, 0);
        }
        esp_io_expander_set_level(io, EXP_BUS_OUT_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(20)); /* let the strip's supply settle before data */
        uint32_t lv = 0;
        esp_io_expander_get_level(io, 0xFFFF, &lv);
        ESP_LOGI(TAG, "bus 5V on: expander pins 0x%04lx", (unsigned long)lv);
    } else {
        esp_io_expander_set_level(io, EXP_BUS_OUT_EN, 0);
        esp_io_expander_set_level(io, EXP_BOOST_EN, 0);
    }
    s_bus_on = on;
}

static void rgb_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num         = RGB_GPIO,
        .max_leds               = RGB_COUNT,
        .led_model              = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "side LEDs not available");
        s_strip = NULL;
    }
}

static void rgb_apply(void)
{
    if (s_strip == NULL) return;
    uint32_t v = s_display_on ? s_rgb_level : 0;
    if (v == 0) {
        led_strip_clear(s_strip);
        bus_5v(false);
        return;
    }
    bus_5v(true);
    uint32_t r = ((s_rgb >> 16) & 0xFF) * v / 255, g = ((s_rgb >> 8) & 0xFF) * v / 255, b = (s_rgb & 0xFF) * v / 255;
    for (int i = 0; i < RGB_COUNT; i++) led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
    esp_err_t err = led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "side LEDs: level %lu rgb %06lx (%s)", v, (unsigned long)s_rgb, esp_err_to_name(err));
}

/* `brightness` is the deck's 0..100 keyboard light level (OFF 0, LOW 3,
 * HIGH 20); the side LEDs take the accent color at a matching level. The
 * Keyboard3's own two LEDs only have on / off. */
void hal_kbd_light(uint8_t brightness, bool use_color, uint32_t rgb)
{
    (void)use_color;
    s_kbd_light = brightness;
    s_rgb       = rgb;
    uint32_t lv = (uint32_t)brightness * 255 / 40;
    s_rgb_level = (uint8_t)(brightness == 0 ? 0 : lv > 255 ? 255 : lv < 8 ? 8 : lv);
    if (s_kbd_present) kbd_light_apply();
    rgb_apply();
}

static void kbd_push(uint8_t code, uint8_t mods, bool pressed)
{
    hal_key_t k = {.code = pressed ? code : 0, .mods = mods, .pressed = pressed};
    if (xQueueSend(s_key_q, &k, 0) != pdTRUE) ESP_LOGW(TAG, "key queue full, dropped 0x%02x", code);
}

/* Polls the key register. The firmware holds the current key's code while
 * it is down and reads 0 once it is up (or clears after one read, which
 * lands on the same press / release pair here). */
static void kbd_task(void *arg)
{
    (void)arg;
    int misses   = 0;
    uint8_t prev = 0, down_code = 0, down_mods = 0;
    for (;;) {
        if (!s_kbd_present) {
            if (kbd_try_attach()) {
                s_kbd_present = true;
                misses        = 0;
                prev          = 0;
                ESP_LOGI(TAG, "keyboard attached, fw %u", (unsigned)s_kbd_fw);
                kbd_light_apply();
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            continue;
        }
        uint8_t raw = 0;
        if (!kbd_read(KBD_REG_KEY, &raw, 1)) {
            if (++misses >= 20) {
                ESP_LOGW(TAG, "keyboard detached");
                s_kbd_present = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        misses = 0;
        if (raw == 0x0D) {
            uint8_t lf = 0;
            kbd_read(KBD_REG_KEY, &lf, 1); /* Enter is followed by a LF byte; eat it */
        }
        if (raw != prev) {
            if (down_code) {
                kbd_push(down_code, down_mods, false);
                down_code = 0;
            }
            uint8_t code, mods;
            if (raw && kbd_translate(raw, &code, &mods)) {
                kbd_push(code, mods, true);
                down_code = code;
                down_mods = mods;
            }
            prev = raw;
        }
        vTaskDelay(pdMS_TO_TICKS(KBD_POLL_MS));
    }
}

bool hal_key_poll(hal_key_t *out) { return s_key_q && xQueueReceive(s_key_q, out, 0) == pdTRUE; }
bool hal_kbd_present(void) { return s_kbd_present; }
uint8_t hal_kbd_fw_version(void) { return s_kbd_fw; }

/* ---- Power: AXP2101 ------------------------------------------------------ */

#define AXP_ADDR 0x34
#define AXP_REG_STATUS1 0x00  /* bit 3: battery present, bit 5: VBUS good */
#define AXP_REG_STATUS2 0x01  /* bits 6:5: 01 charging, 10 discharging */
#define AXP_REG_ADC_EN 0x30   /* bit 0: VBAT */
#define AXP_REG_VBAT_H 0x34   /* 14-bit mV, high 6 bits then 0x35 */
#define AXP_REG_PERCENT 0xA4  /* fuel gauge, 0..100 */

static i2c_master_dev_handle_t s_axp;

static bool axp_read(uint8_t reg, uint8_t *v, size_t n)
{
    return s_axp && i2c_master_transmit_receive(s_axp, &reg, 1, v, n, 50) == ESP_OK;
}

static void axp_init(void)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &s_axp) != ESP_OK) return;
    uint8_t en = 0;
    if (!axp_read(AXP_REG_ADC_EN, &en, 1)) return;
    if (!(en & 0x01)) {
        uint8_t b[2] = {AXP_REG_ADC_EN, (uint8_t)(en | 0x01)};
        i2c_master_transmit(s_axp, b, 2, 50);
    }
    s_axp_ok = true;
}

void hal_power_read(hal_power_t *out)
{
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    if (!s_axp_ok) return;
    uint8_t st0 = 0, st1 = 0, pct = 0, v[2] = {0, 0};
    if (!axp_read(AXP_REG_STATUS1, &st0, 1) || !axp_read(AXP_REG_STATUS2, &st1, 1) ||
        !axp_read(AXP_REG_VBAT_H, v, 2)) {
        return;
    }
    out->valid    = true;
    out->volts    = (float)(((v[0] & 0x3F) << 8) | v[1]) / 1000.0f;
    out->charging = ((st1 >> 5) & 0x03) == 0x01;
    if ((st0 & 0x08) && axp_read(AXP_REG_PERCENT, &pct, 1)) out->percent = pct & 0x7F;
}

/* ---- Clock: BM8563 ------------------------------------------------------- */

#define RTC_ADDR 0x51
#define RTC_REG_CTRL1 0x00
#define RTC_REG_SEC 0x02 /* sec, min, hour, day, weekday, month, year; BCD */

static i2c_master_dev_handle_t s_rtc;

static int bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtc_init(void)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RTC_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &s_rtc) != ESP_OK) return false;
    uint8_t reg = RTC_REG_CTRL1, v = 0xFF;
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, &v, 1, 50) != ESP_OK) return false;
    if (v & 0x20) { /* STOP bit: the clock is not running */
        uint8_t b[2] = {RTC_REG_CTRL1, 0x00};
        i2c_master_transmit(s_rtc, b, 2, 50);
    }
    return true;
}

static bool rtc_read(struct tm *t)
{
    uint8_t reg = RTC_REG_SEC, r[7];
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, r, 7, 50) != ESP_OK) return false;
    if (r[0] & 0x80) return false; /* VL: voltage was low, time is not trustworthy */
    memset(t, 0, sizeof(*t));
    t->tm_sec   = bcd2bin(r[0] & 0x7F);
    t->tm_min   = bcd2bin(r[1] & 0x7F);
    t->tm_hour  = bcd2bin(r[2] & 0x3F);
    t->tm_mday  = bcd2bin(r[3] & 0x3F);
    t->tm_wday  = r[4] & 0x07;
    t->tm_mon   = bcd2bin(r[5] & 0x1F) - 1;
    t->tm_year  = 100 + bcd2bin(r[6]); /* 20xx */
    t->tm_isdst = -1;
    return true;
}

static bool rtc_write(const struct tm *t)
{
    uint8_t b[8] = {RTC_REG_SEC,
                    bin2bcd(t->tm_sec),
                    bin2bcd(t->tm_min),
                    bin2bcd(t->tm_hour),
                    bin2bcd(t->tm_mday),
                    (uint8_t)t->tm_wday,
                    bin2bcd(t->tm_mon + 1),
                    bin2bcd((t->tm_year + 1900) % 100)};
    return i2c_master_transmit(s_rtc, b, sizeof(b), 50) == ESP_OK;
}

/* The RTC holds local wall time, read after the timezone is applied. */
static void rtc_seed_system_time(void)
{
    struct tm t;
    if (!rtc_read(&t)) {
        ESP_LOGW(TAG, "RTC time not valid");
        return;
    }
    if (t.tm_year < 124) { /* before 2024: never set */
        ESP_LOGW(TAG, "RTC not set (%d-%02d-%02d)", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        return;
    }
    struct timeval tv = {mktime(&t), 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "time from RTC: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min);
}

bool hal_rtc_set(const struct tm *local)
{
    struct tm t       = *local;
    time_t epoch      = mktime(&t); /* normalizes fields and fills tm_wday */
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, NULL);
    if (!s_rtc_ok) return false;
    return rtc_write(&t);
}

bool hal_rtc_present(void) { return s_rtc_ok; }

static int s_tz = DECK_TZ_DEFAULT;

static void tz_apply(int index)
{
    s_tz = (index >= 0 && index < DECK_TZ_COUNT) ? index : DECK_TZ_DEFAULT;
    setenv("TZ", g_deck_tz[s_tz].posix, 1);
    tzset();
}

int hal_tz_count(void) { return DECK_TZ_COUNT; }
const char *hal_tz_name(int index) { return (index >= 0 && index < DECK_TZ_COUNT) ? g_deck_tz[index].name : "?"; }
int hal_tz_get(void) { return s_tz; }

void hal_tz_set(int index)
{
    time_t now = time(NULL);
    tz_apply(index);
    hal_cfg_set_i32("tz", s_tz);
    struct tm local;
    localtime_r(&now, &local);
    hal_rtc_set(&local); /* same instant, new wall time */
}

/* ---- Storage ------------------------------------------------------------- */

static char s_sd_err[24];    /* last SD mount error name, "" when mounted */
static char s_flash_err[24]; /* same for the internal FAT partition */

/* The card is on the LCD's SPI bus. A card that fails at 20 MHz sometimes
 * answers at 10, so a failed mount gets one slower retry. After a failed
 * esp_vfs_fat_sdspi_mount the SDSPI device is removed again, so trying
 * again later (a card inserted after boot) is safe. */
static bool sd_write_check(void)
{
    static const char probe[] = HAL_SD_MOUNT "/.coreos";
    FILE *f = fopen(probe, "wb");
    if (f == NULL) return false;
    bool ok = fputs("ok", f) >= 0;
    ok      = (fclose(f) == 0) && ok;
    char back[4] = "";
    f           = fopen(probe, "rb");
    if (f) {
        ok = fgets(back, sizeof(back), f) != NULL && ok;
        fclose(f);
    } else {
        ok = false;
    }
    unlink(probe);
    return ok && strcmp(back, "ok") == 0;
}

bool hal_storage_mount_sd(void)
{
    if (s_sd_ok) return true;
    /* GPIO 35 is the LCD's D/C line and the card's MISO. esp_lcd drives it
     * only during LCD transfers; the rest of the time it must idle high, as
     * the SD protocol expects, or a card that tri-states until it answers
     * leaves the host reading noise. */
    gpio_set_pull_mode(BSP_SD_SPI_MISO, GPIO_PULLUP_ONLY);
    /* The BSP switches the slot's supply on right before CMD0. Give a card
     * that was just powered its ramp and the spec's 1 ms before talking. */
    bsp_feature_enable(BSP_FEATURE_SD, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "SD probe: gpio35 oe=%d level=%d", (int)((GPIO.enable1.val >> 3) & 1), gpio_get_level(BSP_SD_SPI_MISO));
    bool locked = lvgl_port_lock(1000); /* no LCD traffic on the shared bus while the card initialises */
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };
    static const int freqs[] = {SDMMC_FREQ_DEFAULT, 10000};
    esp_err_t err            = ESP_FAIL;
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]) && err != ESP_OK; i++) {
        sdmmc_host_t host;
        bsp_sdcard_get_sdspi_host(SDMMC_HOST_SLOT_0, &host);
        host.max_freq_khz   = freqs[i];
        bsp_sdcard_cfg_t cfg = {.mount = &mcfg, .host = &host};
        err                  = bsp_sdcard_sdspi_mount(&cfg);
        if (err != ESP_OK) ESP_LOGW(TAG, "no SD card at %d kHz (%s)", freqs[i], esp_err_to_name(err));
    }
    if (locked) lvgl_port_unlock();
    if (err == ESP_OK) {
        s_card  = bsp_sdcard_get_handle();
        s_sd_ok = s_card != NULL;
        if (s_card) {
            ESP_LOGI(TAG, "SD mounted: %s, %llu MB", s_card->cid.name,
                     (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size >> 20));
        }
        s_sd_err[0] = '\0';
    } else {
        snprintf(s_sd_err, sizeof(s_sd_err), "%s", esp_err_to_name(err));
    }
    /* A card can mount (one good read of the boot sector) and still corrupt
     * everything after. Prove it with a write before anyone builds on it;
     * otherwise let go of it so the internal flash takes over. The BSP's own
     * unmount would also free the SPI bus the LCD is on, so unmount directly. */
    if (s_sd_ok && !sd_write_check()) {
        ESP_LOGW(TAG, "SD card mounted but a write failed, falling back to flash");
        esp_vfs_fat_sdcard_unmount(HAL_SD_MOUNT, s_card);
        s_card  = NULL;
        s_sd_ok = false;
        snprintf(s_sd_err, sizeof(s_sd_err), "RW FAIL");
    }
    return s_sd_ok;
}

/* Internal flash so notes and the vault work without a card. Mounted
 * regardless of the SD, so a card pulled later still leaves a home. */
#define FLASH_MOUNT "/flash"
static bool s_flash_ok;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

static void flash_fs_mount(void)
{
    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = true,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, "storage", &mcfg, &s_wl);
    s_flash_ok    = err == ESP_OK;
    if (s_flash_ok) {
        s_flash_err[0] = '\0';
    } else {
        snprintf(s_flash_err, sizeof(s_flash_err), "%s", esp_err_to_name(err));
        ESP_LOGW(TAG, "internal storage not mounted (%s)", esp_err_to_name(err));
    }
}

/* Give up on a mounted card that turned out unusable. */
bool hal_storage_drop_sd(void)
{
    if (!s_sd_ok) return false;
    ESP_LOGW(TAG, "dropping the SD card: it stopped answering properly");
    esp_vfs_fat_sdcard_unmount(HAL_SD_MOUNT, s_card);
    s_card  = NULL;
    s_sd_ok = false;
    snprintf(s_sd_err, sizeof(s_sd_err), "RW FAIL");
    return true;
}

const char *hal_storage_root(void)
{
    if (s_sd_ok) return HAL_SD_MOUNT;
    if (s_flash_ok) return FLASH_MOUNT;
    return NULL;
}

/* One line for the SYSTEM screen, error names included so a failure can be
 * read off the deck without a serial console. */
void hal_storage_status(char *buf, size_t n)
{
    /* Free space is a FAT walk; on a big or slow card that stalls the UI
     * task, and SYSTEM asks every second. Remember it for a while. */
    static uint64_t total, free_b;
    static int64_t at = -1;
    static bool was_sd;
    int64_t now = esp_timer_get_time();
    if (at < 0 || now - at > 10 * 1000000 || was_sd != s_sd_ok) {
        total = free_b = 0;
        if (s_sd_ok) {
            esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
        } else if (s_flash_ok) {
            esp_vfs_fat_info(FLASH_MOUNT, &total, &free_b);
        }
        at     = now;
        was_sd = s_sd_ok;
    }
    if (s_sd_ok) {
        snprintf(buf, n, "SD %.1f / %.1f GB FREE", (double)free_b / 1e9, (double)total / 1e9);
        return;
    }
    const char *sd = s_sd_err[0] ? s_sd_err : "NONE";
    if (s_flash_ok) {
        snprintf(buf, n, "SD %s, FLASH %.1f MB FREE", sd, (double)free_b / 1e6);
    } else {
        snprintf(buf, n, "SD %s, FLASH %s", sd, s_flash_err[0] ? s_flash_err : "NONE");
    }
}

bool hal_storage_is_sd(void) { return s_sd_ok; }
bool hal_sd_mounted(void) { return s_sd_ok; }

uint64_t hal_sd_total_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return total;
}

uint64_t hal_sd_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return free_b;
}

/* ---- Audio board control -------------------------------------------------- */

void *hal_sys_i2c(void) { return (void *)bsp_i2c_get_handle(); }

void hal_speaker_amp(bool on) { bsp_feature_enable(BSP_FEATURE_SPEAKER, on); }

bool hal_headphones(void) { return false; } /* no jack on the CoreS3 */

/* ---- Settings ------------------------------------------------------------ */

/* Settings live in the "deck" namespace of the default NVS partition.
 *
 * NVS is encrypted with XTS keys derived (by the HMAC peripheral) from a key
 * in an eFuse key block. On the first boot of a build with this code there is
 * no such key: the plain settings are copied to RAM, a random HMAC key is
 * burned into the first free key block, the partition is erased, re-opened
 * encrypted and the settings are written back. That burn is the only eFuse
 * change and it does not restrict flashing.
 *
 * Anything going wrong on that path falls back to plain NVS, so settings may
 * be lost but the deck always boots. */

#define NVS_PART "nvs"
#define MIGRATE_MAX 48

typedef struct {
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    int32_t i32;
    void *data; /* str (with NUL) or blob */
    size_t len;
} kv_t;

static esp_err_t plain_open(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_init_partition(NVS_PART);
    }
    return err;
}

static esp_err_t secure_open(nvs_sec_cfg_t *cfg)
{
    esp_err_t err = nvs_flash_secure_init_partition(NVS_PART, cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_secure_init_partition(NVS_PART, cfg);
    }
    return err;
}

/* Copy the plain "deck" namespace (already initialised) into RAM. */
static int dump_settings(kv_t *kv, int max)
{
    int n = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, "deck", NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_iterator_t it = NULL;
    esp_err_t err     = nvs_entry_find(NVS_PART, "deck", NVS_TYPE_ANY, &it);
    while (err == ESP_OK && n < max) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        kv_t *e = &kv[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", info.key);
        e->type = info.type;
        bool ok = false;
        if (info.type == NVS_TYPE_I32) {
            ok = nvs_get_i32(h, info.key, &e->i32) == ESP_OK;
        } else if (info.type == NVS_TYPE_STR || info.type == NVS_TYPE_BLOB) {
            bool str = info.type == NVS_TYPE_STR;
            if ((str ? nvs_get_str(h, info.key, NULL, &e->len) : nvs_get_blob(h, info.key, NULL, &e->len)) == ESP_OK &&
                (e->data = malloc(e->len ? e->len : 1)) != NULL) {
                ok = (str ? nvs_get_str(h, info.key, (char *)e->data, &e->len)
                          : nvs_get_blob(h, info.key, e->data, &e->len)) == ESP_OK;
            }
        }
        if (ok) {
            n++;
        } else {
            free(e->data);
            ESP_LOGW(TAG, "settings migration: skipped %s", info.key);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    return n;
}

static void restore_settings(kv_t *kv, int n)
{
    for (int i = 0; i < n; i++) {
        kv_t *e = &kv[i];
        if (e->type == NVS_TYPE_I32) nvs_set_i32(s_nvs, e->key, e->i32);
        if (e->type == NVS_TYPE_STR) nvs_set_str(s_nvs, e->key, (const char *)e->data);
        if (e->type == NVS_TYPE_BLOB) nvs_set_blob(s_nvs, e->key, e->data, e->len);
    }
    nvs_commit(s_nvs);
}

static void wipe_settings(kv_t *kv, int n)
{
    for (int i = 0; i < n; i++) {
        if (kv[i].data) {
            memset(kv[i].data, 0, kv[i].len); /* Wi-Fi password, vault passphrase */
            free(kv[i].data);
        }
    }
    memset(kv, 0, sizeof(kv_t) * (size_t)n);
}

/* Derive the XTS keys from the HMAC key in `blk`; with `generate`, burn a new
 * random HMAC key into it first (it must be unused). */
static esp_err_t sec_cfg(esp_efuse_block_t blk, bool generate, nvs_sec_cfg_t *cfg)
{
    nvs_sec_config_hmac_t hcfg = {.hmac_key_id = (hmac_key_id_t)(blk - EFUSE_BLK_KEY0)};
    nvs_sec_scheme_t *scheme   = NULL;
    esp_err_t err              = nvs_sec_provider_register_hmac(&hcfg, &scheme);
    if (err != ESP_OK) return err;
    err = generate ? nvs_flash_generate_keys_v2(scheme, cfg) : nvs_flash_read_security_cfg_v2(scheme, cfg);
    return err;
}

static void nvs_init(void)
{
    static kv_t kv[MIGRATE_MAX];
    int migrated      = -1; /* entries waiting to be written back, or -1 */
    nvs_sec_cfg_t cfg = {0};
    esp_err_t err     = ESP_FAIL;
    esp_efuse_block_t blk;

    if (esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &blk)) {
        /* Normal boot: the key exists. */
        if (sec_cfg(blk, false, &cfg) == ESP_OK) err = secure_open(&cfg);
        if (err != ESP_OK) ESP_LOGE(TAG, "encrypted settings failed (0x%x), using plain NVS", err);
    } else if ((blk = esp_efuse_find_unused_key_block()) != EFUSE_BLK_KEY_MAX) {
        /* First boot with encryption: migrate. */
        int n = 0;
        if (plain_open() == ESP_OK) {
            n = dump_settings(kv, MIGRATE_MAX);
            nvs_flash_deinit_partition(NVS_PART);
        }
        ESP_LOGW(TAG, "encrypting settings: burning an HMAC key into eFuse key block %d", (int)(blk - EFUSE_BLK_KEY0));
        if (sec_cfg(blk, true, &cfg) == ESP_OK) {
            nvs_flash_erase_partition(NVS_PART);
            err = secure_open(&cfg);
        } else {
            ESP_LOGE(TAG, "could not create the settings key, keeping plain NVS");
        }
        migrated = n;
    } else {
        ESP_LOGW(TAG, "no free eFuse key block, settings stay unencrypted");
    }

    s_nvs_encrypted = err == ESP_OK;
    if (err != ESP_OK) err = plain_open(); /* never fail to boot over settings */
    s_nvs_ok = (err == ESP_OK) && nvs_open_from_partition(NVS_PART, "deck", NVS_READWRITE, &s_nvs) == ESP_OK;

    if (migrated >= 0) {
        /* If the burn failed the plain partition was never erased, and the
         * values are already there; writing them again is harmless. */
        if (s_nvs_ok) restore_settings(kv, migrated);
        ESP_LOGI(TAG, "settings migrated (%d entries, %s)", migrated, s_nvs_encrypted ? "encrypted" : "plain");
        wipe_settings(kv, migrated);
    } else if (s_nvs_encrypted) {
        ESP_LOGI(TAG, "settings encrypted (eFuse key block %d)", (int)(blk - EFUSE_BLK_KEY0));
    }
}

bool hal_cfg_encrypted(void) { return s_nvs_encrypted; }

bool hal_cfg_get_blob(const char *key, void *out, size_t *len)
{
    return s_nvs_ok && nvs_get_blob(s_nvs, key, out, len) == ESP_OK;
}

void hal_cfg_set_blob(const char *key, const void *data, size_t len)
{
    if (!s_nvs_ok) return;
    if (data) {
        nvs_set_blob(s_nvs, key, data, len);
    } else {
        nvs_erase_key(s_nvs, key);
    }
    nvs_commit(s_nvs);
}

int32_t hal_cfg_get_i32(const char *key, int32_t def)
{
    int32_t v = def;
    if (s_nvs_ok && nvs_get_i32(s_nvs, key, &v) != ESP_OK) v = def;
    return v;
}

void hal_cfg_set_i32(const char *key, int32_t value)
{
    if (!s_nvs_ok) return;
    nvs_set_i32(s_nvs, key, value);
    nvs_commit(s_nvs);
}

bool hal_cfg_get_str(const char *key, char *out, size_t n)
{
    if (n) out[0] = '\0';
    size_t len = n;
    return s_nvs_ok && n && nvs_get_str(s_nvs, key, out, &len) == ESP_OK;
}

void hal_cfg_set_str(const char *key, const char *value)
{
    if (!s_nvs_ok) return;
    if (value) {
        nvs_set_str(s_nvs, key, value);
    } else {
        nvs_erase_key(s_nvs, key);
    }
    nvs_commit(s_nvs);
}

/* ---- System info --------------------------------------------------------- */

void hal_sysinfo(hal_sysinfo_t *out)
{
    out->board          = "CORES3 LITE";
    out->chip           = s_chip_name;
    out->cpu_mhz        = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    out->heap_int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->psram_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->uptime_s       = (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---- Init ---------------------------------------------------------------- */

bool deck_hal_init(void)
{
    nvs_init();

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(s_chip_name, sizeof(s_chip_name), "ESP32-S3 rev%d.%d x%d", ci.revision / 100, ci.revision % 100,
             ci.cores);

    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "system I2C failed");
        return false;
    }
    esp_log_level_set("M5Stack Core S3", ESP_LOG_WARN); /* the BSP logs every backlight change */

    bsp_feature_enable(BSP_FEATURE_SD, true); /* card supply up early, mount comes later */

    if (!display_init()) return false;
    touch_tune();
    hal_backlight_set((uint8_t)hal_cfg_get_i32("bright", 80));

    tz_apply((int)hal_cfg_get_i32("tz", DECK_TZ_DEFAULT)); /* before the RTC is read as local time */
    s_rtc_ok = rtc_init();
    if (s_rtc_ok) {
        rtc_seed_system_time();
    } else {
        ESP_LOGW(TAG, "RTC not found");
    }

    axp_init();
    if (!s_axp_ok) ESP_LOGW(TAG, "AXP2101 not found");

    flash_fs_mount();
    hal_storage_mount_sd();

    rgb_init();

    s_key_q = xQueueCreate(64, sizeof(hal_key_t));
    xTaskCreatePinnedToCore(kbd_task, "kbd", 4096, NULL, 4, NULL, 0);
    return true;
}
