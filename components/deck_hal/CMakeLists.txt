idf_component_register(
    SRCS "hal_cores3.c" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES
        espressif__m5stack_core_s3
        espressif__esp_lvgl_port
        espressif__led_strip
        espressif__esp_io_expander
        esp_driver_rmt
        esp_driver_i2c
        esp_driver_gpio
        sdmmc
        fatfs
        wear_levelling
        nvs_flash
        nvs_sec_provider
        efuse
        esp_timer
    WHOLE_ARCHIVE # lv_mem_psram.c defines LVGL's allocator hooks; nothing in this library references them
)
