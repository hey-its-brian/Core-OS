idf_component_register(
    SRCS "player.c" "id3.c" "aout_cores3.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES deck_hal
    PRIV_REQUIRES minimp3 esp_timer espressif__esp_codec_dev espressif__m5stack_core_s3
)
