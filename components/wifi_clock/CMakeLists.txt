idf_component_register(SRCS "wifi_clock.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        esp_wifi
                        esp_event
                        esp_netif
                        nvs_flash
                        lwip
                        freertos
                        driver
                        esp_timer
                        # Wi-Fi provisioning components
                        wifi_provisioning
                        wpa_supplicant
                        esp_common
                        )

# Add compile options
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Werror
    -Wextra
)

# Add conditional compilation for provisioning
if(CONFIG_WIFI_CLOCK_ENABLE_PROVISIONING)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CONFIG_WIFI_CLOCK_ENABLE_PROVISIONING=1
    )
endif()

# Kconfig configuration
set(config_file ${CMAKE_CURRENT_LIST_DIR}/Kconfig.projbuild)
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY COMPONENT_CONFIG_FILE ${config_file})