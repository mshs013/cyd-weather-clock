file(GLOB_RECURSE UI_SOURCES "ui/*.c")
idf_component_register(SRCS "main.c" ${UI_SOURCES}
                    INCLUDE_DIRS "." "ui"
                    PRIV_REQUIRES spi_flash lvgl lvgl_esp32_drivers lv_porting wifi_provisioning wifi_clock backlight weather)

# Add linker flags to remove unused sections
target_link_options(${COMPONENT_LIB} PRIVATE 
    "-Wl,--gc-sections"
    "-Wl,--strip-all"
)