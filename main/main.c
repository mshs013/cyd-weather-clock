#include <stdio.h>
#include <string.h>
#include <math.h> 
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"

#include "lvgl.h"
#include "esp_timer.h"
#include "ui/ui.h"

#include "cyd_display.h"
#include "wifi_clock.h"
#include "weather.h"

static const char *TAG = "MAIN";

// LED pin definitions
#define RED_PIN 4
#define BLUE_PIN 16
#define GREEN_PIN 17

// LED states
typedef enum {
    LED_OFF = 1,
    LED_ON = 0
} led_state_t;

typedef enum {
    LVGL_EVENT_PROVISIONING_SUCCESS,
    LVGL_EVENT_CONNECTION_SUCCESS,
    LVGL_EVENT_PROVISIONING_FAILED,
    // ... other events
} lvgl_event_type_t;

typedef struct {
    lvgl_event_type_t type;
    char message[64];
} lvgl_event_msg_t;

// Add this near your other queue handles
static QueueHandle_t lvgl_event_queue = NULL;

// Global UI objects
static lv_obj_t *label_time = NULL;
static lv_obj_t *label_sec = NULL;
static lv_obj_t *label_ampm = NULL;
static lv_obj_t *label_date = NULL;
static lv_obj_t *label_week = NULL;
static lv_obj_t *label_info = NULL;
static lv_obj_t *forecast_label_info = NULL;
static lv_obj_t *wifi_icon = NULL;
static lv_obj_t *brightness_icon = NULL;  // Brightness mode icon

// FPS and CPU monitoring
static char fps_str[16];
static char cpu_str[16];

// Weather UI object references
static lv_obj_t *label_city = NULL;
static lv_obj_t *label_weather_main = NULL;
static lv_obj_t *label_temperature = NULL;
static lv_obj_t *label_feels_like = NULL;
static lv_obj_t *label_temp_max = NULL;
static lv_obj_t *label_temp_min = NULL;
static lv_obj_t *label_humidity = NULL;
static lv_obj_t *label_wind_dir = NULL;
static lv_obj_t *label_wind = NULL;
static lv_obj_t *label_pressure = NULL;
static lv_obj_t *label_visibility = NULL;
static lv_obj_t *weather_icon = NULL;

// Track last displayed update times
static time_t last_displayed_weather_update = 0;
static time_t last_displayed_forecast_update = 0;
static time_t last_displayed_astro_update = 0;

// Track current astronomical data
static astronomical_data_t current_astro_data = {0};

// Track day for (N) indicator updates
static int current_day_of_year = -1;
static int current_year = -1;

// Semaphore for thread-safe display updates
static SemaphoreHandle_t display_mutex = NULL;

// Timer handles
static esp_timer_handle_t astro_indicator_timer = NULL;
static esp_timer_handle_t brightness_adjust_timer = NULL;

// Track astronomical display state
static uint32_t last_astro_display_state = 0;
static bool astro_initial_display_done = false;

// Track initialization state
static bool weather_module_initialized = false;

// Wi-Fi Clock status tracking
static wifi_clock_status_t last_wifi_status = WIFI_CLOCK_STATUS_DISCONNECTED;

// Wi-Fi global variables
#define WIFI_EVENT_QUEUE_SIZE 10
static QueueHandle_t wifi_event_queue = NULL;

typedef struct {
    wifi_clock_event_t event;
    char info_text[32];
} wifi_event_msg_t;

// Function prototypes
static void init_leds(void);
void set_initial_display_of_labels(void);
static void update_weather_icon(lv_obj_t *icon_obj, const char *icon_code, bool is_day);
static int time_to_seconds_of_day(time_t timestamp);
static uint32_t calculate_astro_display_state(void);
static void update_astro_display_with_indicators(bool force_update);
static void astro_indicator_timer_callback(void *arg);
static void brightness_adjust_timer_callback(void *arg);
static void update_weather_display_if_needed(void);
static void update_forecast_display_if_needed(void);
static void update_astronomical_display_if_needed(void);
static void update_fps_cpu_display(void);
static void update_clock_display(const wifi_clock_time_data_t *time_data);
static void inc_lvgl_tick(void *arg);
static void lvgl_task(void *arg);
static void check_and_update_astro_indicators(void);
static void debug_weather_timestamps(void);
static void wifi_clock_event_handler(wifi_clock_event_t event, void *data);
static void update_brightness_icon(void);
static void brightness_mode_event_handler(void);
static uint32_t seconds_until_next_hour(void);
static const char* get_current_time_string(void);
static void provisioning_event_callback(wifi_clock_event_t event, void* arg);
static void process_lvgl_event(lvgl_event_msg_t *msg);
static void handle_provisioning_success(void);
static void handle_connection_success(void);
static void update_wifi_status_label(wifi_clock_status_t status);

/**
 * @brief Calculate seconds until next hour at 00 minutes
 */
static uint32_t seconds_until_next_hour(void) {
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Calculate seconds until next hour's 00 minutes
    uint32_t seconds_until = (60 - timeinfo.tm_min) * 60 - timeinfo.tm_sec;
    if (seconds_until == 0) {
        seconds_until = 3600; // If exactly at hour, wait 1 hour
    }
    
    return seconds_until;
}

/**
 * @brief Get current time as string for logging
 */
static const char* get_current_time_string(void) {
    static char time_str[20];
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
    
    return time_str;
}

void ui_event_brightness(lv_event_t * e)
{
    lv_obj_t * bar = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    // Only process if we're in manual mode
    if(cyd_display_get_backlight_mode() != CYD_BACKLIGHT_MODE_MANUAL) {
        return;
    }

    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
    {
        lv_point_t p;
        lv_indev_t * indev = lv_indev_get_act();
        lv_indev_get_point(indev, &p);

        /* Get bar coordinates */
        lv_area_t bar_area;
        lv_obj_get_coords(bar, &bar_area);

        /* Calculate value based on X position */
        int32_t min = 20; //lv_bar_get_min_value(bar);
        int32_t max = lv_bar_get_max_value(bar);

        int32_t value = (p.x - bar_area.x1) * (max - min)
                        / (bar_area.x2 - bar_area.x1)
                        + min;

        /* Clamp value */
        if(value < min) value = min;
        if(value > max) value = max;

        lv_bar_set_value(bar, value, LV_ANIM_OFF);
        
        // Set brightness (only in manual mode)
        cyd_display_set_backlight_brightness(value);
        
        // Save settings to NVS
        cyd_display_save_brightness_to_nvs(value);

        if (label_info != NULL) {
            char info_buf[64];
            snprintf(info_buf, sizeof(info_buf), "Manual: Brightness set to %d%%", cyd_display_get_backlight_brightness());
            lv_label_set_text(label_info, info_buf);
        }
    }
}

void ui_event_imageiconbrightness(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);

    if(event_code == LV_EVENT_PRESSED) {
        // Save current mode before toggling
        cyd_backlight_mode_t previous_mode = cyd_display_get_backlight_mode();
        
        // Toggle the mode
        cyd_display_toggle_auto_mode();
        
        // Get the new mode
        cyd_backlight_mode_t new_mode = cyd_display_get_backlight_mode();
        
        // Only update if mode actually changed
        if (previous_mode != new_mode) {
            ESP_LOGI(TAG, "Brightness mode changed from %d to %d", previous_mode, new_mode);
            
            // Update the icon
            update_brightness_icon();
            
            // Get current brightness
            int current_brightness = cyd_display_get_backlight_brightness();
            
            // Update the UI bar - with mutex protection
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Check if bar object still exists and is valid
                if (ui_brightness != NULL && lv_obj_is_valid(ui_brightness)) {
                    lv_bar_set_value(ui_brightness, current_brightness, LV_ANIM_ON);
                    
                    // Optional: Update info label
                    if (label_info != NULL) {
                        char info_buf[64];
                        if (new_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
                            snprintf(info_buf, sizeof(info_buf), "Auto brightness: %d%%", current_brightness);
                        } else {
                            snprintf(info_buf, sizeof(info_buf), "Manual brightness: %d%%", current_brightness);
                        }
                        lv_label_set_text(label_info, info_buf);
                    }
                }
                xSemaphoreGive(display_mutex);
            } else {
                ESP_LOGW(TAG, "Failed to acquire display mutex for brightness update");
            }
            
            // Save the new mode to NVS
            cyd_display_save_mode_to_nvs(new_mode);
            
            // If switching to auto mode, apply brightness immediately
            if (new_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
                cyd_display_adjust_brightness_for_time();
            }
        } else {
            ESP_LOGW(TAG, "Mode toggle didn't change the mode");
        }
    }
}

// Then update the ui_event_imageiconwifi function:
void ui_event_imageiconwifi(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED)
        return;

    ESP_LOGI(TAG, "WiFi icon pressed");

    /* Prevent double start */
    if (wifi_clock_is_provisioning())
    {
        ESP_LOGW(TAG, "Provisioning already running");
        return;
    }
    
    /* Prevent provisioning if Wi-Fi connected */
    if (wifi_clock_is_connected())
    {
        ESP_LOGW(TAG, "Wi-Fi already Connected");
        if (label_info != NULL) {
            lv_label_set_text(label_info, "Wi-Fi already Connected");
        }
        return;
    }

    /* STOP wifi_clock */
    if (wifi_clock_get_status() == WIFI_CLOCK_STATUS_CONNECTING)
    {
        ESP_LOGI(TAG, "Stopping wifi_clock before provisioning");
        wifi_clock_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Configure provisioning */
    wifi_clock_prov_config_t prov_config = {
        .method = WIFI_CLOCK_PROV_SOFT_AP,
        .service_name = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_SSID,
        .service_password = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_PASSWORD,
        .max_retries = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_MAX_RETRIES,
        .timeout_ms = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_TIMEOUT_MS,
        .reset_provisioned = false
    };

    /* Register callback for provisioning events */
    wifi_clock_register_callback(provisioning_event_callback);

    /* Change screen - LVGL 9.3 still uses similar screen functions */
    _ui_screen_change(&ui_Provision,
                      LV_SCR_LOAD_ANIM_FADE_ON,
                      500, 0,
                      &ui_Provision_screen_init);

    vTaskDelay(pdMS_TO_TICKS(100));

    /* Start provisioning */
    esp_err_t ret = wifi_clock_start_provisioning(&prov_config);

    if (ret == ESP_OK)
    {
        lv_label_set_text(ui_labelprovisioninfo,
                      "Connect to WiFi: " CONFIG_WIFI_CLOCK_PROV_SOFT_AP_SSID);

        // ===== FIXED: Use configured values directly =====
        // Wait for provisioning to fully start
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Use configured values directly
        const char *service_name = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_SSID;
        const char *pop_str = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_PASSWORD;
        
        ESP_LOGI(TAG, "Service Name: %s, POP: %s", service_name, pop_str);
        
        // Generate QR code payload
        char payload[256];
        if (strlen(pop_str) == 0) {
            snprintf(payload, sizeof(payload), 
                    "{\"ver\":\"v1\",\"name\":\"%s\",\"transport\":\"softap\"}",
                    service_name);
        } else {
            snprintf(payload, sizeof(payload), 
                    "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
                    service_name, pop_str);
        }
        
        ESP_LOGI(TAG, "QR Code Payload: %s", payload);
        
        // Update the QR code - LVGL 9.3 qrcode functions remain the same
        if (ui_qrcode != NULL) {
            lv_res_t res = lv_qrcode_update(ui_qrcode, payload, strlen(payload));
            
            if (res == LV_RES_OK) {
                ESP_LOGI(TAG, "QR code updated successfully");
                
                // Add SSID label
                static lv_obj_t *ssid_label = NULL;
                if (ssid_label == NULL) {
                    ssid_label = lv_label_create(ui_Panel2);
                    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xF0F0F0), 0);
                    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_12, 0);
                }
                
                char ssid_display[64];
                snprintf(ssid_display, sizeof(ssid_display), "SSID: %s", service_name);
                lv_label_set_text(ssid_label, ssid_display);
                lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, 110);
            } else {
                ESP_LOGE(TAG, "Failed to generate QR code");
            }
        }
    }
    else
    {
        lv_label_set_text(ui_labelprovisioninfo,
                          "Provision start failed");
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
    }
}

// Add this callback function implementation
static void provisioning_event_callback(wifi_clock_event_t event, void* arg)
{
    ESP_LOGI(TAG, "Provisioning callback - event: %d", event);
    
    // Verify UI objects exist
    if (ui_labelprovisioninfo == NULL) {
        ESP_LOGE(TAG, "ui_labelprovisioninfo is NULL!");
    }
    
    if (lvgl_event_queue == NULL) {
        ESP_LOGE(TAG, "lvgl_event_queue is NULL!");
    }

    switch (event)
    {
        case WIFI_CLOCK_EVENT_PROVISIONING_STARTED:
            ESP_LOGI(TAG, "Provisioning started");
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Waiting for credentials...");
            }
            break;

        case WIFI_CLOCK_EVENT_PROVISIONING_SUCCESS:
            ESP_LOGI(TAG, "Provisioning SUCCESS");

            // Temporarily disable auto-reconnect to prevent multiple attempts
            wifi_clock_set_auto_reconnect(false);
            
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Provisioning successful! Returning to main...");
            }

            /* Queue event for LVGL task */
            if (lvgl_event_queue) {
                lvgl_event_msg_t msg = {
                    .type = LVGL_EVENT_PROVISIONING_SUCCESS
                };
                strlcpy(msg.message, "Provisioning successful!", sizeof(msg.message));
                
                // Use a small timeout to ensure it's sent
                if (xQueueSend(lvgl_event_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to queue provisioning success event");
                } else {
                    ESP_LOGI(TAG, "Queued provisioning success event");
                }
            } else {
                ESP_LOGE(TAG, "LVGL event queue is NULL");
            }
            break;

        case WIFI_CLOCK_EVENT_PROVISIONING_FAILED:
            ESP_LOGW(TAG, "Provisioning FAILED");
            
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Provisioning failed. Press WiFi icon to retry.");
            }

            if (wifi_event_queue)
            {
                wifi_event_msg_t msg = {
                    .event = WIFI_CLOCK_EVENT_DISCONNECTED
                };
                strlcpy(msg.info_text,
                        "Provisioning failed",
                        sizeof(msg.info_text));
                xQueueSend(wifi_event_queue, &msg, 0);
            }
            
            /* Queue event for LVGL task */
            if (lvgl_event_queue) {
                lvgl_event_msg_t msg = {
                    .type = LVGL_EVENT_PROVISIONING_FAILED
                };
                strlcpy(msg.message, "Provisioning failed", sizeof(msg.message));
                xQueueSend(lvgl_event_queue, &msg, 0);
            }
            break;

        case WIFI_CLOCK_EVENT_PROVISIONING_STOPPED:
            ESP_LOGI(TAG, "Provisioning stopped");
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Provisioning stopped");
            }
            break;

        case WIFI_CLOCK_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected after provisioning");
            
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Connected!");
            }

            // Force Wi-Fi icon update immediately
            if (wifi_icon) {
                lv_image_set_src(wifi_icon, &ui_img_icon_wifion_24p_png); // LVGL 9.3 uses lv_image_set_src
                lv_obj_invalidate(wifi_icon);
                ESP_LOGI(TAG, "WiFi icon set to ON");
            }
            
            /* Queue connection success for LVGL task */
            if (lvgl_event_queue) {
                lvgl_event_msg_t msg = {
                    .type = LVGL_EVENT_CONNECTION_SUCCESS
                };
                strlcpy(msg.message, "Connected to WiFi", sizeof(msg.message));
                xQueueSend(lvgl_event_queue, &msg, 0);
            }
            break;

        case WIFI_CLOCK_EVENT_CONNECTION_FAILED:
            ESP_LOGW(TAG, "Connection failed after provisioning");
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo,
                                  "Connection failed. Check credentials.");
            }
            break;

        case WIFI_CLOCK_EVENT_TIME_SYNCED:
            ESP_LOGI(TAG, "Time synced after provisioning");
            
            // Force Wi-Fi icon to ON when time is synced
            if (wifi_icon) {
                lv_image_set_src(wifi_icon, &ui_img_icon_wifion_24p_png); // LVGL 9.3 uses lv_image_set_src
                lv_obj_invalidate(wifi_icon);
                ESP_LOGI(TAG, "WiFi icon set to ON (time synced)");
            }
            break;
            
        default:
            break;
    }
}

/**
 * @brief Update brightness mode icon based on current mode
 */
static void update_brightness_icon(void) {
    if (brightness_icon == NULL) {
        return;
    }
    
    cyd_backlight_mode_t current_mode = cyd_display_get_backlight_mode();
    
    if (current_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        // Auto mode - show auto brightness icon
        lv_image_set_src(brightness_icon, &ui_img_icon_auto_brightness_16p_png); // LVGL 9.3 uses lv_image_set_src
        ESP_LOGI(TAG, "Brightness icon set to AUTO mode");
    } else {
        // Manual mode (or any other mode) - show manual brightness icon
        lv_image_set_src(brightness_icon, &ui_img_icon_man_brightness_16p_png); // LVGL 9.3 uses lv_image_set_src
        ESP_LOGI(TAG, "Brightness icon set to MANUAL mode");
    }
}

/**
 * @brief Handle brightness mode changes
 */
static void brightness_mode_event_handler(void) {
    cyd_backlight_mode_t current_mode = cyd_display_get_backlight_mode();
    
    ESP_LOGI(TAG, "Brightness mode changed to: %s", 
             (current_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) ? "AUTO" : "MANUAL");
    
    // Update the icon
    update_brightness_icon();
    
    // If switching to manual mode, disable auto brightness timer
    if (current_mode == CYD_BACKLIGHT_MODE_MANUAL) {
        ESP_LOGI(TAG, "Manual mode: Auto brightness disabled");
    } else if (current_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        ESP_LOGI(TAG, "Auto mode: Auto brightness enabled");
        
        // Immediately adjust brightness for current time
        cyd_display_adjust_brightness_for_time();
    }
}

/**
 * @brief Check if a time string represents a later time than another
 */
bool is_datetime_greater(const char* new_time, const char* last_displayed_time) {
    int new_h, new_m, new_s, last_h, last_m, last_s;
    
    if (sscanf(new_time, "%d:%d:%d", &new_h, &new_m, &new_s) != 3) return false;
    if (sscanf(last_displayed_time, "%d:%d:%d", &last_h, &last_m, &last_s) != 3) return true;
    
    int new_secs = new_h * 3600 + new_m * 60 + new_s;
    int last_secs = last_h * 3600 + last_m * 60 + last_s;
    
    return new_secs > last_secs;
}

/**
 * @brief Initialize GPIO pins for LEDs
 */
static void init_leds(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RED_PIN) | (1ULL << GREEN_PIN) | (1ULL << BLUE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    gpio_config(&io_conf);
    
    gpio_set_level(RED_PIN, LED_OFF);
    gpio_set_level(GREEN_PIN, LED_OFF);
    gpio_set_level(BLUE_PIN, LED_OFF);
    
    ESP_LOGI(TAG, "LEDs initialized");
}

/**
 * @brief Timer callback to adjust brightness based on time of day (only in auto mode)
 * This timer reschedules itself to run at the next hour
 */
static void brightness_adjust_timer_callback(void *arg) {
    cyd_backlight_mode_t current_mode = cyd_display_get_backlight_mode();
    int current_brightness = cyd_display_get_backlight_brightness();
    
    ESP_LOGD(TAG, "Brightness timer fired. Current mode: %d", current_mode);
    
    // Always log that timer fired, even if not in auto mode
    ESP_LOGI(TAG, "Hourly brightness check triggered at %s", 
             get_current_time_string());
    
    // Only adjust brightness if we're in auto mode
    if (current_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        ESP_LOGI(TAG, "Auto brightness adjustment triggered at hour change");
        
        // Adjust brightness based on time of day
        cyd_display_adjust_brightness_for_time();
        
        int adjusted_brightness = cyd_display_get_backlight_brightness();

        if(current_brightness != adjusted_brightness) {
            // Update the UI bar - with mutex protection
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Check if bar object still exists and is valid
                if (ui_brightness != NULL && lv_obj_is_valid(ui_brightness)) {
                    lv_bar_set_value(ui_brightness, adjusted_brightness, LV_ANIM_ON);
                }
                
                // Update info label
                if (label_info != NULL && lv_obj_is_valid(label_info)) {
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Auto brightness set to %d%% (hourly)", adjusted_brightness);
                    lv_label_set_text(label_info, info_buf);
                }
                xSemaphoreGive(display_mutex);
            }
            
            ESP_LOGI(TAG, "Auto brightness changed from %d%% to %d%% at hour change", 
                     current_brightness, adjusted_brightness);
        } else {
            ESP_LOGI(TAG, "Auto brightness unchanged at %d%%", adjusted_brightness);
        }
    } else {
        // In manual mode, just log that timer fired but no action taken
        ESP_LOGD(TAG, "Brightness timer fired but mode is MANUAL - no action");
    }
    
    // Reschedule the timer for the next hour
    uint32_t seconds_to_next_hour = seconds_until_next_hour();
    esp_timer_start_once(brightness_adjust_timer, seconds_to_next_hour * 1000000);
    ESP_LOGI(TAG, "Next brightness check scheduled in %lu seconds", seconds_to_next_hour);
}

/**
 * @brief Set initial display of all labels
 */
void set_initial_display_of_labels(void) {
    if (label_city != NULL) lv_label_set_text(label_city, "Dhaka");
    if (label_date != NULL) lv_label_set_text(label_date, "---, -- --- ----");
    if (label_time != NULL) lv_label_set_text(label_time, "--:--");
    if (label_sec != NULL) lv_label_set_text(label_sec, "--");
    if (label_ampm != NULL) lv_label_set_text(label_ampm, "--");
    if (label_week != NULL) lv_label_set_text(label_week, "WEEK --");
    if (label_weather_main != NULL) lv_label_set_text(label_weather_main, "--");
    if (label_temperature != NULL) lv_label_set_text(label_temperature, "---°C");
    if (label_feels_like != NULL) lv_label_set_text(label_feels_like, "--- °C");
    if (label_temp_max != NULL) lv_label_set_text(label_temp_max, "--- °C");
    if (label_temp_min != NULL) lv_label_set_text(label_temp_min, "--- °C");
    if (label_humidity != NULL) lv_label_set_text(label_humidity, "--%");
    if (label_wind_dir != NULL) lv_label_set_text(label_wind_dir, "Wind (--)");
    if (label_wind != NULL) lv_label_set_text(label_wind, "-- km/h");
    if (label_pressure != NULL) lv_label_set_text(label_pressure, "-- hPa");
    if (label_visibility != NULL) lv_label_set_text(label_visibility, "--%");
    if (label_info != NULL) lv_label_set_text(label_info, "Starting...");
    
    // Set initial brightness icon
    if (brightness_icon != NULL) {
        update_brightness_icon();
    }
    
    // Forecast data
    for (int i = 0; i < 5; i++) {
        lv_label_set_text(ui_ForecastWidgets[i].date_label, "--/--");
        lv_label_set_text(ui_ForecastWidgets[i].main_label, "N/A");
        lv_label_set_text(ui_ForecastWidgets[i].temp_label, "--/--°C");
        lv_image_set_src(ui_ForecastWidgets[i].icon_image, &ui_img_icon_01d_72p_png); // LVGL 9.3 uses lv_image_set_src
    }
    if (forecast_label_info != NULL) lv_label_set_text(forecast_label_info, "------");
    
    // Astronomical data
    lv_label_set_text(ui_labelsunrise, "--:--");
    lv_label_set_text(ui_labelsunset, "--:--");
    lv_label_set_text(ui_labelmoonrise, "--:--");
    lv_label_set_text(ui_labelmoonset, "--:--");
    lv_image_set_src(ui_iconmoon, &ui_img_icon_new_moon_32p_png); // LVGL 9.3 uses lv_image_set_src
}

/**
 * @brief Update weather icon based on icon code
 */
static void update_weather_icon(lv_obj_t *icon_obj, const char *icon_code, bool is_day) {
    if (icon_obj == NULL) return;
    
    if (strcmp(icon_code, "01d") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_01d_72p_png);
    } else if (strcmp(icon_code, "01n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_01n_72p_png);
    } else if (strcmp(icon_code, "02d") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_02d_72p_png);
    } else if (strcmp(icon_code, "02n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_02n_72p_png);
    } else if (strcmp(icon_code, "03d") == 0 || strcmp(icon_code, "03n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_03d_03n_72p_png);
    } else if (strcmp(icon_code, "04d") == 0 || strcmp(icon_code, "04n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_04d_04n_72p_png);
    } else if (strcmp(icon_code, "09d") == 0 || strcmp(icon_code, "09n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_09d_09n_72p_png);
    } else if (strcmp(icon_code, "10d") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_10d_72p_png);
    } else if (strcmp(icon_code, "10n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_10n_72p_png);
    } else if (strcmp(icon_code, "11d") == 0 || strcmp(icon_code, "11n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_11d_11n_72p_png);
    } else if (strcmp(icon_code, "13d") == 0 || strcmp(icon_code, "13n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_13d_13n_72p_png);
    } else if (strcmp(icon_code, "50d") == 0 || strcmp(icon_code, "50n") == 0) {
        lv_image_set_src(icon_obj, &ui_img_icon_50d_50n_72p_png);
    } else {
        lv_image_set_src(icon_obj, is_day ? &ui_img_icon_01d_72p_png : &ui_img_icon_01n_72p_png);
    }
}

/**
 * @brief Convert timestamp to time of day in seconds since midnight
 */
static int time_to_seconds_of_day(time_t timestamp) {
    if (timestamp == 0) return -1;
    
    struct tm tm_time;
    localtime_r(&timestamp, &tm_time);
    return (tm_time.tm_hour * 3600) + (tm_time.tm_min * 60) + tm_time.tm_sec;
}

/**
 * @brief Calculate current astronomical display state
 */
static uint32_t calculate_astro_display_state(void) {
    if (!current_astro_data.valid) {
        return 0;
    }
    
    uint32_t state = 0;
    time_t current_time;
    struct tm now;
    
    time(&current_time);
    localtime_r(&current_time, &now);
    
    struct tm today_start = now;
    today_start.tm_hour = 0;
    today_start.tm_min = 0;
    today_start.tm_sec = 0;
    time_t today_start_time = mktime(&today_start);
    
    int current_seconds = time_to_seconds_of_day(current_time);
    
    // Check sunrise
    if (current_astro_data.sunrise >= today_start_time) {
        int sunrise_seconds = time_to_seconds_of_day(current_astro_data.sunrise);
        if (sunrise_seconds >= 0 && sunrise_seconds < current_seconds) {
            state |= (1 << 0);
        }
    }
    
    // Check sunset
    if (current_astro_data.sunset >= today_start_time) {
        int sunset_seconds = time_to_seconds_of_day(current_astro_data.sunset);
        if (sunset_seconds >= 0 && sunset_seconds < current_seconds) {
            state |= (1 << 1);
        }
    }
    
    // Check moonrise
    if (current_astro_data.moonrise >= today_start_time) {
        int moonrise_seconds = time_to_seconds_of_day(current_astro_data.moonrise);
        if (moonrise_seconds >= 0 && moonrise_seconds < current_seconds) {
            state |= (1 << 2);
        }
    }
    
    // Check moonset
    if (current_astro_data.moonset >= today_start_time) {
        int moonset_seconds = time_to_seconds_of_day(current_astro_data.moonset);
        if (moonset_seconds >= 0 && moonset_seconds < current_seconds) {
            state |= (1 << 3);
        }
    }
    
    return state;
}

/**
 * @brief Update astronomical display with proper (N) indicators
 */
static void update_astro_display_with_indicators(bool force_update) {
    if (!current_astro_data.valid) {
        return;
    }
    
    uint32_t current_state = calculate_astro_display_state();
    
    if (!force_update && astro_initial_display_done && 
        current_state == last_astro_display_state) {
        return;
    }
    
    char time_buf[32];
    char display_buf[64];
    bool show_next;
    
    // Update sunrise
    bool success = weather_format_astronomical_time_display(
        current_astro_data.sunrise, 
        current_astro_data.next_sunrise, 
        time_buf, sizeof(time_buf), 
        &show_next);
    
    if (!success) {
        // Get current time format from wifi_clock
        wifi_clock_time_data_t time_data;
        if (wifi_clock_get_time_data(&time_data)) {
            // Use the same format as the clock display
            strcpy(time_buf, "--:--");
        } else {
            strcpy(time_buf, "--:--");
        }
    }
    
    bool show_sunrise_n = (current_state & (1 << 0)) != 0;
    if (show_sunrise_n) {
        snprintf(display_buf, sizeof(display_buf), "%s (N)", time_buf);
        lv_label_set_text(ui_labelsunrise, display_buf);
    } else {
        lv_label_set_text(ui_labelsunrise, time_buf);
    }
    
    // Update sunset
    success = weather_format_astronomical_time_display(
        current_astro_data.sunset, 
        current_astro_data.next_sunset, 
        time_buf, sizeof(time_buf), 
        &show_next);
    
    if (!success) {
        wifi_clock_time_data_t time_data;
        if (wifi_clock_get_time_data(&time_data)) {
            strcpy(time_buf, "--:--");
        } else {
            strcpy(time_buf, "--:--");
        }
    }
    
    bool show_sunset_n = (current_state & (1 << 1)) != 0;
    if (show_sunset_n) {
        snprintf(display_buf, sizeof(display_buf), "%s (N)", time_buf);
        lv_label_set_text(ui_labelsunset, display_buf);
    } else {
        lv_label_set_text(ui_labelsunset, time_buf);
    }
    
    // Update moonrise
    success = weather_format_astronomical_time_display(
        current_astro_data.moonrise, 
        current_astro_data.next_moonrise, 
        time_buf, sizeof(time_buf), 
        &show_next);
    
    if (!success) {
        wifi_clock_time_data_t time_data;
        if (wifi_clock_get_time_data(&time_data)) {
            strcpy(time_buf, "--:--");
        } else {
            strcpy(time_buf, "--:--");
        }
    }
    
    bool show_moonrise_n = (current_state & (1 << 2)) != 0;
    if (show_moonrise_n) {
        snprintf(display_buf, sizeof(display_buf), "%s (N)", time_buf);
        lv_label_set_text(ui_labelmoonrise, display_buf);
    } else {
        lv_label_set_text(ui_labelmoonrise, time_buf);
    }
    
    // Update moonset
    success = weather_format_astronomical_time_display(
        current_astro_data.moonset, 
        current_astro_data.next_moonset, 
        time_buf, sizeof(time_buf), 
        &show_next);
    
    if (!success) {
        wifi_clock_time_data_t time_data;
        if (wifi_clock_get_time_data(&time_data)) {
            strcpy(time_buf, "--:--");
        } else {
            strcpy(time_buf, "--:--");
        }
    }
    
    bool show_moonset_n = (current_state & (1 << 3)) != 0;
    if (show_moonset_n) {
        snprintf(display_buf, sizeof(display_buf), "%s (N)", time_buf);
        lv_label_set_text(ui_labelmoonset, display_buf);
    } else {
        lv_label_set_text(ui_labelmoonset, time_buf);
    }
    
    // Update moon icon
    if (current_astro_data.moon_phase < 0.125f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_waning_crescent_32p_png);
    } else if (current_astro_data.moon_phase < 0.25f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_last_quarter_32p_png);
    } else if (current_astro_data.moon_phase < 0.375f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_waning_gibbous_32p_png);
    } else if (current_astro_data.moon_phase < 0.5f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_full_moon_32p_png);
    } else if (current_astro_data.moon_phase < 0.625f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_waxing_gibbous_32p_png);
    } else if (current_astro_data.moon_phase < 0.75f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_first_quarter_32p_png);
    } else if (current_astro_data.moon_phase < 0.875f) {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_waxing_crescent_32p_png);
    } else {
        lv_image_set_src(ui_iconmoon, &ui_img_icon_new_moon_32p_png);
    }

    last_astro_display_state = current_state;
    astro_initial_display_done = true;
}

/**
 * @brief Timer callback to update (N) indicators periodically
 */
static void astro_indicator_timer_callback(void *arg) {
    if (current_astro_data.valid) {
        time_t current_time;
        struct tm now;
        time(&current_time);
        localtime_r(&current_time, &now);
        
        bool day_changed = (current_year != now.tm_year || current_day_of_year != now.tm_yday);
        
        if (day_changed) {
            current_year = now.tm_year;
            current_day_of_year = now.tm_yday;
            
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                update_astro_display_with_indicators(true);
                xSemaphoreGive(display_mutex);
            }
        }
    }
}

/**
 * @brief Check and update astro indicators if needed
 */
static void check_and_update_astro_indicators(void) {
    if (!current_astro_data.valid) {
        return;
    }
    
    uint32_t current_state = calculate_astro_display_state();
    
    // Only update if state has changed
    if (current_state != last_astro_display_state) {
        ESP_LOGI(TAG, "Astro state changed: 0x%02x -> 0x%02x", last_astro_display_state, current_state);
        
        if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            update_astro_display_with_indicators(false);
            xSemaphoreGive(display_mutex);
        }
    }
}

/**
 * @brief Debug weather timestamps
 */
static void debug_weather_timestamps(void) {
    time_t last_update = weather_get_last_update_time();
    
    char last_update_str[32] = "Never";
    char displayed_str[32] = "Never";
    
    if (last_update > 0) {
        struct tm tm_info;
        localtime_r(&last_update, &tm_info);
        strftime(last_update_str, sizeof(last_update_str), "%H:%M:%S", &tm_info);
    }
    
    if (last_displayed_weather_update > 0) {
        struct tm tm_info;
        localtime_r(&last_displayed_weather_update, &tm_info);
        strftime(displayed_str, sizeof(displayed_str), "%H:%M:%S", &tm_info);
    }
    
    ESP_LOGD(TAG, "Weather Timestamps - Last update: %s, Displayed: %s, Flags: W=%d F=%d A=%d",
             last_update_str, displayed_str,
             weather_has_new_weather_data(), 
             weather_has_new_forecast_data(), 
             weather_has_new_astro_data());
}

/**
 * @brief Process Wi-Fi event queue (called from LVGL task)
 */
static void process_wifi_event_queue(void) {
    wifi_event_msg_t msg;
    
    while (wifi_event_queue != NULL && xQueueReceive(wifi_event_queue, &msg, 0) == pdTRUE) {
        if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Update Wi-Fi icon
            if (wifi_icon != NULL && lv_obj_is_valid(wifi_icon)) {
                const void *icon_src = &ui_img_icon_wifioff_24p_png;
                
                if (msg.event == WIFI_CLOCK_EVENT_CONNECTED || 
                    msg.event == WIFI_CLOCK_EVENT_GOT_IP || 
                    msg.event == WIFI_CLOCK_EVENT_TIME_SYNCED) {
                    icon_src = &ui_img_icon_wifion_24p_png;
                }
                
                lv_image_set_src(wifi_icon, icon_src); // LVGL 9.3 uses lv_image_set_src
                lv_obj_invalidate(wifi_icon);
            }
            
            // Update info label
            if (label_info != NULL && lv_obj_is_valid(label_info)) {
                lv_label_set_text(label_info, msg.info_text);
                lv_obj_invalidate(label_info);
            }
            
            // If this is a time sync event and in auto mode, update brightness bar
            if (msg.event == WIFI_CLOCK_EVENT_TIME_SYNCED && 
                cyd_display_get_backlight_mode() == CYD_BACKLIGHT_MODE_AUTO_TIME) {
                
                int adjusted_brightness = cyd_display_get_backlight_brightness();
                
                // Update brightness bar
                if (ui_brightness != NULL && lv_obj_is_valid(ui_brightness)) {
                    lv_bar_set_value(ui_brightness, adjusted_brightness, LV_ANIM_ON);
                }
                
                // Update info label to show brightness adjustment
                if (label_info != NULL && lv_obj_is_valid(label_info)) {
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Time synced - Auto brightness: %d%%", 
                             adjusted_brightness);
                    lv_label_set_text(label_info, info_buf);
                }
                
                ESP_LOGI(TAG, "Brightness bar updated after time sync to %d%%", adjusted_brightness);
            }
            
            xSemaphoreGive(display_mutex);
        }
    }
}

/**
 * @brief Wi-Fi Clock event handler - NOW SAFE (only queues events)
 */
static void wifi_clock_event_handler(wifi_clock_event_t event, void *data) {
    ESP_LOGI(TAG, "Wi-Fi Clock event: %d - queueing for UI update", event);
    
    // Create queue if not exists
    if (wifi_event_queue == NULL) {
        wifi_event_queue = xQueueCreate(WIFI_EVENT_QUEUE_SIZE, sizeof(wifi_event_msg_t));
        if (wifi_event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi event queue");
            return;
        }
    }
    
    wifi_event_msg_t msg;
    msg.event = event;
    
    // Track last event - initialize to invalid value
    static wifi_clock_event_t last_event = (wifi_clock_event_t)-1;
    static char last_info_text[64] = "";
    
    // Only set info_text if the event has changed
    if (event != last_event) {
        switch (event) {
            case WIFI_CLOCK_EVENT_CONNECTED:
                strlcpy(msg.info_text, "Wi-Fi connected", sizeof(msg.info_text));
                break;
            case WIFI_CLOCK_EVENT_GOT_IP:
                strlcpy(msg.info_text, "Got IP address", sizeof(msg.info_text));
                break;
            case WIFI_CLOCK_EVENT_TIME_SYNCED:
                strlcpy(msg.info_text, "Time synced", sizeof(msg.info_text));
                
                // Trigger immediate brightness update when time sync occurs
                if (cyd_display_get_backlight_mode() == CYD_BACKLIGHT_MODE_AUTO_TIME) {
                    ESP_LOGI(TAG, "Time synced - triggering immediate brightness adjustment");
                    
                    // Adjust brightness immediately
                    cyd_display_adjust_brightness_for_time();
                    
                    // The UI update will be handled in process_wifi_event_queue
                }
                break;
                
            case WIFI_CLOCK_EVENT_TIME_SYNC_FAILED:
                strlcpy(msg.info_text, "Time sync failed", sizeof(msg.info_text));
                break;
            case WIFI_CLOCK_EVENT_DISCONNECTED:
                strlcpy(msg.info_text, "Wi-Fi disconnected", sizeof(msg.info_text));
                break;
            case WIFI_CLOCK_EVENT_CONNECTION_FAILED:
                strlcpy(msg.info_text, "Wi-Fi connection failed", sizeof(msg.info_text));
                break;
            default:
                strlcpy(msg.info_text, "Wi-Fi event", sizeof(msg.info_text));
                break;
        }
        last_event = event;
        strlcpy(last_info_text, msg.info_text, sizeof(last_info_text));
        
        // Queue the message
        xQueueSend(wifi_event_queue, &msg, 0);
        ESP_LOGI(TAG, "Queued event: %s", msg.info_text);
    } else {
        ESP_LOGD(TAG, "Duplicate event %d - skipping", event);
    }
}

/**
 * @brief Update weather display only if new data is available
 */
static void update_weather_display_if_needed(void) {
    if (!weather_module_initialized || !weather_is_enabled()) {
        return;
    }
    
    // Check if new data is available using multiple methods
    bool new_data_available = false;
    
    // Method 1: Check new data flags
    if (weather_has_new_weather_data()) {
        ESP_LOGI(TAG, "New weather data detected via flag");
        new_data_available = true;
    }
    
    // Method 2: Check specific weather changed flag
    if (weather_is_weather_changed()) {
        ESP_LOGI(TAG, "Weather data changed via callback");
        new_data_available = true;
    }
    
    // Method 3: Check timestamp (backup method)
    time_t last_weather_update = weather_get_last_update_time();
    if (last_weather_update > 0 && last_weather_update > last_displayed_weather_update) {
        ESP_LOGI(TAG, "New weather data detected via timestamp");
        new_data_available = true;
    }
    
    // Debug output
    debug_weather_timestamps();
    
    if (new_data_available) {
        weather_data_t weather;
        
        if (weather_get_data(&weather) && weather.valid) {
            ESP_LOGI(TAG, "NEW Weather data confirmed! Updating display...");
            
            // Update the display timestamp
            last_displayed_weather_update = last_weather_update > 0 ? last_weather_update : time(NULL);
            
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Update display elements
                if (label_weather_main != NULL) {
                    lv_label_set_text(label_weather_main, weather_get_description(weather.weather_code));
                }
                
                if (label_temperature != NULL) {
                    char temp_str[16];
                    snprintf(temp_str, sizeof(temp_str), "%.0f°C", weather.temperature);
                    lv_label_set_text(label_temperature, temp_str);
                }
                
                if (label_feels_like != NULL) {
                    char feels_str[16];
                    snprintf(feels_str, sizeof(feels_str), "%.0f °C", weather.feels_like);
                    lv_label_set_text(label_feels_like, feels_str);
                }
                
                if (label_temp_max != NULL) {
                    char temp_max_str[16];
                    snprintf(temp_max_str, sizeof(temp_max_str), "%.0f °C", weather.temp_max);
                    lv_label_set_text(label_temp_max, temp_max_str);
                }
                
                if (label_temp_min != NULL) {
                    char temp_min_str[16];
                    snprintf(temp_min_str, sizeof(temp_min_str), "%.0f °C", weather.temp_min);
                    lv_label_set_text(label_temp_min, temp_min_str);
                }
                
                if (label_humidity != NULL) {
                    char humidity_str[16];
                    snprintf(humidity_str, sizeof(humidity_str), "%.0f%%", weather.humidity);
                    lv_label_set_text(label_humidity, humidity_str);
                }
                
                if (label_wind_dir != NULL) {
                    char wind_dir_str[32];
                    const char* direction = weather_get_wind_direction(weather.wind_direction);
                    snprintf(wind_dir_str, sizeof(wind_dir_str), "Wind (%s)", direction);
                    lv_label_set_text(label_wind_dir, wind_dir_str);
                }
                
                if (label_wind != NULL) {
                    char wind_str[16];
                    snprintf(wind_str, sizeof(wind_str), "%.0f km/h", weather.wind_speed);
                    lv_label_set_text(label_wind, wind_str);
                }
                
                if (label_pressure != NULL) {
                    char pressure_str[16];
                    snprintf(pressure_str, sizeof(pressure_str), "%.0f hPa", weather.pressure);
                    lv_label_set_text(label_pressure, pressure_str);
                }
                
                if (label_visibility != NULL) {
                    char visibility_str[16];
                    snprintf(visibility_str, sizeof(visibility_str), "%.0f%%", weather.rain_probability);
                    lv_label_set_text(label_visibility, visibility_str);
                }
                
                // Update weather icon
                if (weather_icon != NULL) {
                    const char* icon_name = weather_get_icon(weather.weather_code, weather.is_day);
                    update_weather_icon(weather_icon, icon_name, weather.is_day);
                }

                // Update info label
                if (label_info != NULL) {
                    char info_buf[64];
                    if (weather.last_update[0] != '\0') {
                        snprintf(info_buf, sizeof(info_buf), "Weather updated: %s (Succeed)", weather.last_update);
                    } else {
                        struct tm timeinfo;
                        localtime_r(&last_displayed_weather_update, &timeinfo);
                        strftime(info_buf, sizeof(info_buf), "Weather updated: %H:%M:%S", &timeinfo);
                    }
                    lv_label_set_text(label_info, info_buf);
                }
                
                // Clear new data flag after successful display update
                weather_clear_new_data_flags();
                
                xSemaphoreGive(display_mutex);

                if(last_displayed_weather_update > 0){
                    char last_display_str[32];
                    struct tm tm_info;
                    localtime_r(&last_displayed_weather_update, &tm_info);
                    strftime(last_display_str, sizeof(last_display_str), "%H:%M:%S", &tm_info);

                    ESP_LOGI(TAG, "Weather display updated successfully at %s", last_display_str);
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to get valid weather data");
        }
    }
}

/**
 * @brief Update forecast display only if new data is available
 */
static void update_forecast_display_if_needed(void) {
    if (!weather_module_initialized || !weather_is_enabled()) {
        return;
    }
    
    // Check if new data is available using multiple methods
    bool new_data_available = false;
    
    // Method 1: Check new data flags
    if (weather_has_new_forecast_data()) {
        ESP_LOGI(TAG, "New forecast data detected via flag");
        new_data_available = true;
    }
    
    // Method 2: Check specific forecast changed flag
    if (weather_is_forecast_changed()) {
        ESP_LOGI(TAG, "Forecast data changed via callback");
        new_data_available = true;
    }
    
    // Method 3: Check timestamp (backup method)
    time_t last_forecast_update = weather_get_last_forecast_update_time();
    if (last_forecast_update > 0 && last_forecast_update > last_displayed_forecast_update) {
        ESP_LOGI(TAG, "New forecast data detected via timestamp");
        new_data_available = true;
    }
    
    if (new_data_available) {
        weather_forecast_t forecast;
        
        if (weather_get_forecast(&forecast) && forecast.valid) {
            ESP_LOGI(TAG, "NEW Forecast data confirmed! Updating display...");
            
            // Update the display timestamp
            last_displayed_forecast_update = last_forecast_update > 0 ? last_forecast_update : time(NULL);
            
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < forecast.num_days && i < 5; i++) {
                    if (forecast.days[i].valid) {
                        lv_label_set_text(ui_ForecastWidgets[i].date_label, forecast.days[i].date);
                        lv_label_set_text(ui_ForecastWidgets[i].main_label, 
                                         weather_get_description(forecast.days[i].weather_code));
                        
                        char temp_str[32];
                        snprintf(temp_str, sizeof(temp_str), "%.0f/%.0f°C", 
                                forecast.days[i].temp_max, forecast.days[i].temp_min);
                        lv_label_set_text(ui_ForecastWidgets[i].temp_label, temp_str);
                        
                        const char* icon_name = weather_get_icon(forecast.days[i].weather_code, forecast.days[i].is_day);
                        update_weather_icon(ui_ForecastWidgets[i].icon_image, icon_name, forecast.days[i].is_day);
                    }
                }

                // Update info label with last update time
                if (forecast_label_info != NULL) {
                    char info_buf[80];
                    if (forecast.last_update[0] != '\0') {
                        snprintf(info_buf, sizeof(info_buf), "Forecast updated: %s (Succeed)", forecast.last_update);
                    } else {
                        struct tm timeinfo;
                        localtime_r(&last_displayed_forecast_update, &timeinfo);
                        strftime(info_buf, sizeof(info_buf), "Forecast updated: %H:%M:%S", &timeinfo);
                    }
                    lv_label_set_text(forecast_label_info, info_buf);
                }
                
                // Clear new data flag after successful display update
                weather_clear_new_data_flags();
                
                xSemaphoreGive(display_mutex);
                if(last_displayed_forecast_update > 0){
                    char last_display_str[32];
                    struct tm tm_info;
                    localtime_r(&last_displayed_forecast_update, &tm_info);
                    strftime(last_display_str, sizeof(last_display_str), "%H:%M:%S", &tm_info);

                    ESP_LOGI(TAG, "Forecast display updated successfully at %s", last_display_str);
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to get valid forecast data");
        }
    }
}

/**
 * @brief Update astronomical display only if new data is available
 */
static void update_astronomical_display_if_needed(void) {
    if (!weather_module_initialized || !weather_astronomical_is_enabled()) {
        return;
    }
    
    // Check if new data is available using multiple methods
    bool new_data_available = false;
    
    // Method 1: Check new data flags
    if (weather_has_new_astro_data()) {
        ESP_LOGI(TAG, "New astronomical data detected via flag");
        new_data_available = true;
    }
    
    // Method 2: Check specific astro changed flag
    if (weather_is_astro_changed()) {
        ESP_LOGI(TAG, "Astronomical data changed via callback");
        new_data_available = true;
    }
    
    // Method 3: Check timestamp (backup method)
    time_t last_astro_update = weather_get_last_astronomical_update_time();
    if (last_astro_update > 0 && 
        (!astro_initial_display_done || last_astro_update > last_displayed_astro_update)) {
        ESP_LOGI(TAG, "New astronomical data detected via timestamp");
        new_data_available = true;
    }
    
    if (new_data_available) {
        astronomical_data_t astro;
        
        if (weather_get_astronomical_data(&astro) && astro.valid) {
            ESP_LOGI(TAG, "NEW Astronomical data confirmed! Updating display...");
            
            // Update the display timestamp
            last_displayed_astro_update = last_astro_update > 0 ? last_astro_update : time(NULL);
            memcpy(&current_astro_data, &astro, sizeof(astronomical_data_t));
            
            if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                update_astro_display_with_indicators(true);
                
                // Clear new data flag after successful display update
                weather_clear_new_data_flags();
                
                xSemaphoreGive(display_mutex);
                if(last_displayed_astro_update > 0){
                    if(last_displayed_weather_update > 0){
                        char last_display_str[32];
                        struct tm tm_info;
                        localtime_r(&last_displayed_weather_update, &tm_info);
                        strftime(last_display_str, sizeof(last_display_str), "%H:%M:%S", &tm_info);

                        if (label_info != NULL && is_datetime_greater(astro.last_update, last_display_str)) {
                            char info_buf[64];
                            if (astro.last_update[0] != '\0') {
                                snprintf(info_buf, sizeof(info_buf), "Astro updated: %s (Succeed)", astro.last_update);
                            } else {
                                struct tm timeinfo;
                                localtime_r(&last_displayed_astro_update, &timeinfo);
                                strftime(info_buf, sizeof(info_buf), "Astro updated: %H:%M:%S", &timeinfo);
                            }
                            lv_label_set_text(label_info, info_buf);
                        }
                    }
                    char last_display_astro_str[32];
                    struct tm tm_info;
                    localtime_r(&last_displayed_astro_update, &tm_info);
                    strftime(last_display_astro_str, sizeof(last_display_astro_str), "%H:%M:%S", &tm_info);
                    ESP_LOGI(TAG, "Astronomical display updated successfully at %s", last_display_astro_str);
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to get valid astronomical data");
        }
    }
}

/**
 * @brief Update FPS and CPU display
 */
static void update_fps_cpu_display(void) {
    static uint32_t fps_last_tick = 0;
    static uint32_t fps_frame_count = 0;
    static uint32_t fps_value = 0;
    
    uint32_t current_tick = lv_tick_get();
    fps_frame_count++;
    
    if (current_tick - fps_last_tick >= 1000) {
        fps_value = (fps_frame_count * 1000) / (current_tick - fps_last_tick);
        fps_frame_count = 0;
        fps_last_tick = current_tick;
        
        if (ui_Labelfps != NULL) {
            snprintf(fps_str, sizeof(fps_str), "%lu", fps_value);
            lv_label_set_text(ui_Labelfps, fps_str);
        }
        
        if (ui_Labelcpu != NULL) {
            uint32_t cpu_percent = (fps_value * 100) / 60;
            if (cpu_percent > 100) cpu_percent = 100;
            snprintf(cpu_str, sizeof(cpu_str), "%lu%%", cpu_percent);
            lv_label_set_text(ui_Labelcpu, cpu_str);
        }
    }
}

/**
 * @brief Update clock display from time data
 */
static void update_clock_display(const wifi_clock_time_data_t *time_data) {
    if (time_data == NULL) return;
    
    if (label_time != NULL) {
        lv_label_set_text(label_time, time_data->time);
    }
    
    if (label_sec != NULL) {
        lv_label_set_text(label_sec, time_data->seconds);
    }
    
    if (label_ampm != NULL) {
        if (time_data->is_24h_format || time_data->ampm[0] == '\0') {
            lv_label_set_text(label_ampm, "");
        } else {
            lv_label_set_text(label_ampm, time_data->ampm);
        }
    }
    
    if (label_date != NULL) {
        lv_label_set_text(label_date, time_data->date);
    }
    
    if (label_week != NULL) {
        char week_str[16];
        snprintf(week_str, sizeof(week_str), "WEEK %02d", time_data->week_number);
        lv_label_set_text(label_week, week_str);
    }
}

/**
 * @brief LVGL tick timer callback
 */
static void inc_lvgl_tick(void *arg) {
    lv_tick_inc(10);
}

/**
 * @brief Weather update callback handler
 */
static void weather_update_callback_handler(void) {
    ESP_LOGI(TAG, "Weather update callback received - forcing display update");
}

static void process_lvgl_event(lvgl_event_msg_t *msg) {
    switch (msg->type) {
        case LVGL_EVENT_PROVISIONING_SUCCESS:
            handle_provisioning_success();
            break;
        case LVGL_EVENT_CONNECTION_SUCCESS:
            handle_connection_success();
            break;
        case LVGL_EVENT_PROVISIONING_FAILED:
            if (ui_labelprovisioninfo) {
                lv_label_set_text(ui_labelprovisioninfo, msg->message);
            }
            break;
        default:
            break;
    }
}

static void handle_provisioning_success(void) {
    ESP_LOGI(TAG, "LVGL: Processing provisioning success");
    
    if (wifi_icon) {
        lv_image_set_src(wifi_icon, &ui_img_icon_wifioff_24p_png);
        lv_obj_invalidate(wifi_icon);
    }
    
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    ESP_LOGI(TAG, "LVGL: Changing to main screen");
    _ui_screen_change(&ui_Main,
                     LV_SCR_LOAD_ANIM_FADE_ON,
                     500, 0,
                     &ui_Main_screen_init);
    
    vTaskDelay(pdMS_TO_TICKS(100));
    if (wifi_icon) {
        bool is_connected = wifi_clock_is_connected() || wifi_clock_is_time_synced();
        const void *icon_src = is_connected ? &ui_img_icon_wifion_24p_png : &ui_img_icon_wifioff_24p_png;
        lv_image_set_src(wifi_icon, icon_src);
        lv_obj_invalidate(wifi_icon);
        ESP_LOGI(TAG, "LVGL: Main screen - WiFi icon set to %s", is_connected ? "ON" : "OFF");
    }
    wifi_clock_set_auto_reconnect(true);
}

static void handle_connection_success(void) {
    ESP_LOGI(TAG, "LVGL: Connection success after provisioning");
    
    if (wifi_icon) {
        lv_image_set_src(wifi_icon, &ui_img_icon_wifion_24p_png);
        lv_obj_invalidate(wifi_icon);
        ESP_LOGI(TAG, "LVGL: WiFi icon set to ON");
    }
    
    if (lv_scr_act() == ui_Provision) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        _ui_screen_change(&ui_Main,
                         LV_SCR_LOAD_ANIM_FADE_ON,
                         500, 0,
                         &ui_Main_screen_init);
    }
}

static void update_wifi_status_label(wifi_clock_status_t status) {
    if (display_mutex != NULL && xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (label_info != NULL) {
            const char *status_text;
            switch (status) {
                case WIFI_CLOCK_STATUS_DISCONNECTED:
                    status_text = "Wi-Fi: Disconnected";
                    break;
                case WIFI_CLOCK_STATUS_CONNECTING:
                    status_text = "Wi-Fi: Connecting...";
                    break;
                case WIFI_CLOCK_STATUS_CONNECTED:
                    status_text = "Wi-Fi: Connected";
                    break;
                case WIFI_CLOCK_STATUS_TIME_SYNCED:
                    status_text = "Wi-Fi: Time Synced";
                    break;
                case WIFI_CLOCK_STATUS_PROVISIONING:
                    status_text = "Wi-Fi: Provisioning...";
                    break;
                case WIFI_CLOCK_STATUS_ERROR:
                    status_text = "Wi-Fi: Error";
                    break;
                default:
                    status_text = "Wi-Fi: Unknown";
                    break;
            }
            lv_label_set_text(label_info, status_text);
        }
        xSemaphoreGive(display_mutex);
    }
}

/**
 * @brief LVGL task
 */
static void lvgl_task(void *arg) {
    ESP_LOGI(TAG, "LVGL task started");

    uint8_t saved_brightness = (uintptr_t)arg;
    ESP_LOGI(TAG, "Task received saved brightness: %d%%", saved_brightness);
    
    // Create mutex for thread-safe display updates
    display_mutex = xSemaphoreCreateMutex();
    if (display_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        vTaskDelete(NULL);
        return;
    }

    // Create Wi-Fi event queue
    wifi_event_queue = xQueueCreate(WIFI_EVENT_QUEUE_SIZE, sizeof(wifi_event_msg_t));
    if (wifi_event_queue == NULL) {
        ESP_LOGW(TAG, "Failed to create Wi-Fi event queue, continuing without it");
    }

    // Create LVGL event queue
    lvgl_event_queue = xQueueCreate(5, sizeof(lvgl_event_msg_t));
    if (lvgl_event_queue == NULL) {
        ESP_LOGW(TAG, "Failed to create LVGL event queue");
    }
    
    // Initialize LVGL
    lv_init();

    // Initialize display and backlight
    cyd_display_init();
    ESP_LOGI(TAG, "Backlight initialized, current brightness: %d%%", cyd_display_get_backlight_brightness());
    
    // IMPORTANT: Set the saved brightness
    cyd_display_set_backlight_brightness(saved_brightness);
    cyd_display_save_brightness_to_nvs(saved_brightness);
    
    // Get current backlight mode and handle initial setup
    cyd_backlight_mode_t initial_mode = cyd_display_get_backlight_mode();
    if (initial_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        // If starting in auto mode, apply time-based brightness
        cyd_display_adjust_brightness_for_time();
        ESP_LOGI(TAG, "Initial auto brightness applied");
    }
    
    // Create LVGL tick timer
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &inc_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 10 * 1000));
    
    // Initialize UI
    ui_init();

    // CRITICAL: Set the LVGL bar to the saved brightness value
    if (ui_brightness != NULL) {
        lv_bar_set_value(ui_brightness, cyd_display_get_backlight_brightness(), LV_ANIM_OFF);
        ESP_LOGI(TAG, "LVGL bar set to %d%%", cyd_display_get_backlight_brightness());
    } else {
        ESP_LOGE(TAG, "ui_brightness is NULL!");
    }
    
    // Get UI object references
    label_time = ui_labeltime;
    label_sec = ui_labelsec;
    label_ampm = ui_labelampm;
    label_date = ui_labeldate;
    label_week = ui_labelweek;
    label_info = ui_labelinfo;
    forecast_label_info = ui_labelforecastinfo;
    wifi_icon = ui_imageiconwifi;
    brightness_icon = ui_imageiconbrightness;
    
    // Get weather UI object references
    label_city = ui_labelcity;
    label_weather_main = ui_labelweathermain;
    label_temperature = ui_labeltemperature;
    label_feels_like = ui_labelfeelslike;
    label_temp_max = ui_labeltempmax;
    label_temp_min = ui_labeltempmin;
    label_humidity = ui_labelhumidity;
    label_wind_dir = ui_labelwinddir;
    label_wind = ui_labelwind;
    label_pressure = ui_labelpressure;
    label_visibility = ui_labelvisibility;
    weather_icon = ui_imageiconweather;
    
    // Set initial display state
    set_initial_display_of_labels();
    
    // Log screen info
    lv_display_t* disp = lv_display_get_default();
    int screen_width = lv_display_get_horizontal_resolution(disp);
    int screen_height = lv_display_get_vertical_resolution(disp);
    ESP_LOGI(TAG, "Screen resolution: %dx%d", screen_width, screen_height);
    
    // Update info label with mode and brightness
    if (label_info != NULL) {
        char info_buf[64];
        cyd_backlight_mode_t mode = cyd_display_get_backlight_mode();
        if (mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
            snprintf(info_buf, sizeof(info_buf), "System Ready - Auto Brightness: %d%%", cyd_display_get_backlight_brightness());
        } else {
            snprintf(info_buf, sizeof(info_buf), "System Ready - Manual Brightness: %d%%", cyd_display_get_backlight_brightness());
        }
        lv_label_set_text(label_info, info_buf);
    }
    
    // Initialize Wi-Fi Clock
    esp_err_t ret = wifi_clock_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi Clock");
    } else {
        // Register callback for Wi-Fi events
        wifi_clock_register_callback(wifi_clock_event_handler);
        
        // Start Wi-Fi connection
        wifi_clock_start();
        
        ESP_LOGI(TAG, "Wi-Fi Clock initialized and started");
    }
    
    // Initialize weather module
    weather_init();
    weather_module_initialized = weather_is_initialized();
    
    // Set weather update callback
    weather_set_update_callback(weather_update_callback_handler);
    
    ESP_LOGI(TAG, "Weather module initialized: %d", weather_module_initialized);
    
    // Initialize current day tracking
    time_t current_time = time(NULL);
    if (current_time > 0) {
        struct tm now;
        localtime_r(&current_time, &now);
        current_year = now.tm_year;
        current_day_of_year = now.tm_yday;

        char ready_msg[32];
        strftime(ready_msg, sizeof(ready_msg), "System Ready: %H:%M:%S", &now);
        if (label_info != NULL) {  
            lv_label_set_text(label_info, ready_msg);
            lv_obj_invalidate(label_info);
        }
    }
    
    // Create timer for (N) indicator updates (every 30 seconds)
    const esp_timer_create_args_t astro_indicator_timer_args = {
        .callback = &astro_indicator_timer_callback,
        .name = "astro_indicator_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&astro_indicator_timer_args, &astro_indicator_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(astro_indicator_timer, 30 * 1000000));

    // Create timer for automatic brightness adjustment
    const esp_timer_create_args_t brightness_adjust_timer_args = {
        .callback = &brightness_adjust_timer_callback,
        .name = "brightness_adjust_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&brightness_adjust_timer_args, &brightness_adjust_timer));
    
    // Calculate time until next hour and start timer
    uint32_t seconds_to_next_hour = seconds_until_next_hour();
    ESP_ERROR_CHECK(esp_timer_start_once(brightness_adjust_timer, seconds_to_next_hour * 1000000));
    ESP_LOGI(TAG, "Brightness timer scheduled in %lu seconds (next hour)", seconds_to_next_hour);
    
    // Force an immediate brightness check if in auto mode
    if (initial_mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        cyd_display_adjust_brightness_for_time();
        if (ui_brightness != NULL) {
            lv_bar_set_value(ui_brightness, cyd_display_get_backlight_brightness(), LV_ANIM_ON);
        }
        ESP_LOGI(TAG, "Initial brightness check performed");
    }
    
    ESP_LOGI(TAG, "LVGL initialized, entering main loop");
    
    // Track last update times for various operations
    TickType_t last_clock_update = 0;
    TickType_t last_weather_check = 0;
    TickType_t last_astro_check = 0;
    TickType_t last_brightness_check = 0;
    cyd_backlight_mode_t last_brightness_mode = CYD_BACKLIGHT_MODE_MAX;
    
    // Main LVGL loop with proper yielding
    while (1) {
        // Process LVGL with time monitoring
        uint32_t start_time = xTaskGetTickCount();
        lv_timer_handler();
        uint32_t elapsed = xTaskGetTickCount() - start_time;
        
        // If LVGL took too long, log it
        if (elapsed > pdMS_TO_TICKS(15)) {
            ESP_LOGD(TAG, "LVGL processing took %lu ms", elapsed * portTICK_PERIOD_MS);
        }
        
        // Process any pending Wi-Fi events
        process_wifi_event_queue();
        
        // Process LVGL event queue
        lvgl_event_msg_t lvgl_msg;
        while (lvgl_event_queue && xQueueReceive(lvgl_event_queue, &lvgl_msg, 0) == pdTRUE) {
            process_lvgl_event(&lvgl_msg);
        }
        
        TickType_t current_ticks = xTaskGetTickCount();
        
        // Update clock display every 100ms
        if (current_ticks - last_clock_update >= pdMS_TO_TICKS(100)) {
            last_clock_update = current_ticks;
            
            wifi_clock_time_data_t time_data;
            if (wifi_clock_get_time_data(&time_data) == ESP_OK) {
                update_clock_display(&time_data);
            }
            
            // Check Wi-Fi Clock status changes
            wifi_clock_status_t current_status = wifi_clock_get_status();
            if (current_status != last_wifi_status) {
                last_wifi_status = current_status;
                update_wifi_status_label(current_status);
            }
            
            // Update FPS display
            update_fps_cpu_display();
        }
        
        // Check for weather updates every 2 seconds
        if (current_ticks - last_weather_check >= pdMS_TO_TICKS(2000)) {
            last_weather_check = current_ticks;
            
            update_weather_display_if_needed();
            update_forecast_display_if_needed();
            update_astronomical_display_if_needed();
        }
        
        // Check for (N) indicator updates every second
        if (current_ticks - last_astro_check >= pdMS_TO_TICKS(1000)) {
            last_astro_check = current_ticks;
            check_and_update_astro_indicators();
        }
        
        // Check for brightness mode changes every 500ms
        if (current_ticks - last_brightness_check >= pdMS_TO_TICKS(500)) {
            last_brightness_check = current_ticks;
            
            cyd_backlight_mode_t current_brightness_mode = cyd_display_get_backlight_mode();
            if (current_brightness_mode != last_brightness_mode) {
                last_brightness_mode = current_brightness_mode;
                brightness_mode_event_handler();
            }
        }
        
        // CRITICAL: Yield to other tasks - this prevents watchdog timeout
        // Use a dynamic delay based on how long LVGL took
        if (elapsed < pdMS_TO_TICKS(10)) {
            vTaskDelay(pdMS_TO_TICKS(10 - (elapsed * portTICK_PERIOD_MS)));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting application...");

    // Initialize LEDs
    init_leds();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erasing NVS and reinitializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load saved brightness
    uint8_t saved_brightness = cyd_display_load_brightness_from_nvs();
    ESP_LOGI(TAG, "Loaded brightness from NVS: %d%%", saved_brightness);
    
    // Create LVGL task on CORE 0 with increased stack size
    BaseType_t result = xTaskCreatePinnedToCore(
        lvgl_task,
        "lvgl",
        40960,  // Increased stack size
        (void*)(uintptr_t)saved_brightness,
        2,
        NULL,
        0
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return;
    }
    
    ESP_LOGI(TAG, "Application started");
}