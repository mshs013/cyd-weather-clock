idf_component_register(
    SRCS "cyd_display.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd esp_lcd_st7796 esp_lcd_touch_xpt2046 driver nvs_flash
)