idf_component_register(
    SRCS "backlight.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl_esp32_drivers lv_porting nvs_flash
)