idf_component_register(
    SRCS "app_camera.c" "cam_jpeg.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal deck_net
    PRIV_REQUIRES espressif__esp_jpeg
)
