idf_component_register(
    SRCS "weather.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer freertos esp_http_client mbedtls wifi_clock
)

# Add optimization flags
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Os  # Optimize for size
    -ffunction-sections 
    -fdata-sections
    -Wl,--gc-sections)

# Reduce stack usage warnings
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-stack-usage)