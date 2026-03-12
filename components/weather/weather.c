#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <cJSON.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "weather.h"
#include "wifi_clock.h"
#include "sdkconfig.h"

static const char *TAG = "WEATHER";

// Only include weather functionality if enabled in config
#ifdef CONFIG_WEATHER_ENABLE

#define WEATHER_TASK_STACK CONFIG_WEATHER_TASK_STACK_SIZE

// Memory debugging
static size_t get_free_heap_size(void) {
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

// Global variables
static weather_data_t weather_data = {0};
static astronomical_data_t astronomical_data = {0};
static weather_forecast_t weather_forecast = {0};
static SemaphoreHandle_t weather_mutex = NULL;
static volatile bool weather_update_in_progress_flag = false;
static volatile bool forecast_update_in_progress_flag = false;
static volatile bool astronomical_update_in_progress_flag = false;
static volatile bool is_initialized = false;
static volatile bool waiting_for_time_sync = true;
static int8_t last_check_day = -1;

// Time tracking for updates
static time_t last_weather_update = 0;
static time_t last_forecast_update = 0;
static time_t last_astronomical_update = 0;
static time_t last_weather_interval_update = 0;
static time_t next_scheduled_forecast_update = 0;
static time_t next_scheduled_astronomical_update = 0;

// Callback notification mechanism
static weather_update_callback_t update_callback = NULL;

// Separate change flags for each data type
static bool weather_changed_flag = false;
static bool forecast_changed_flag = false;
static bool astro_changed_flag = false;

// Separate timestamps for each data type
static time_t last_weather_change_time = 0;
static time_t last_forecast_change_time = 0;
static time_t last_astro_change_time = 0;

// New data flags for main.c to detect updates
static volatile bool has_new_weather_data = false;
static volatile bool has_new_forecast_data = false;
static volatile bool has_new_astro_data = false;

// Retry counters for exponential backoff
static int weather_retry_count = 0;
static int forecast_retry_count = 0;
static int astro_retry_count = 0;
static const int MAX_RETRIES = 2;
static time_t last_retry_time = 0;

// Configuration
static struct {
    double latitude;
    double longitude;
    char timezone[64];
    char api_key[128];
    char city[64];
    char astro_timezone[64];
    bool enabled;
    bool astro_enabled;
    int weather_interval_minutes;
    int forecast_update_hour;
    int forecast_update_minute;
    int astronomical_update_hour;
    int astronomical_update_minute;
} weather_config;

// Weather code mappings
typedef struct {
    uint8_t code;
    const char* description;
    const char* icon_day;
    const char* icon_night;
} weather_map_t;

static const weather_map_t weather_map[] = {
    {0, "Clear sky", "01d", "01n"},
    {1, "Mainly clear", "02d", "02n"},
    {2, "Partly cloudy", "03d", "03n"},
    {3, "Overcast", "04d", "04n"},
    {45, "Fog", "50d", "50n"},
    {48, "Fog", "50d", "50n"},
    {51, "Light drizzle", "09d", "09n"},
    {53, "Moderate drizzle", "09d", "09n"},
    {55, "Dense drizzle", "09d", "09n"},
    {56, "Light freezing drizzle", "13d", "13n"},
    {57, "Dense freezing drizzle", "13d", "13n"},
    {61, "Slight rain", "10d", "10n"},
    {63, "Moderate rain", "10d", "10n"},
    {65, "Heavy rain", "10d", "10n"},
    {66, "Light freezing rain", "13d", "13n"},
    {67, "Heavy freezing rain", "13d", "13n"},
    {71, "Slight snow fall", "13d", "13n"},
    {73, "Moderate snow fall", "13d", "13n"},
    {75, "Heavy snow fall", "13d", "13n"},
    {77, "Snow grains", "13d", "13n"},
    {80, "Slight rain showers", "09d", "09n"},
    {81, "Moderate rain showers", "09d", "09n"},
    {82, "Violent rain showers", "09d", "09n"},
    {85, "Slight snow showers", "13d", "13n"},
    {86, "Heavy snow showers", "13d", "13n"},
    {95, "Thunderstorm", "11d", "11n"},
    {96, "Thunderstorm with slight hail", "11d", "11n"},
    {99, "Thunderstorm with heavy hail", "11d", "11n"}
};

// Moon phase data
static const char* moon_phase_names[] = {
    "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
    "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent"
};

static const char* moon_phase_icons[] = {
    "new_moon", "waxing_crescent", "first_quarter", "waxing_gibbous",
    "full_moon", "waning_gibbous", "last_quarter", "waning_crescent"
};

// Task handles
static TaskHandle_t weather_task_handle = NULL;
static TaskHandle_t forecast_task_handle = NULL;
static TaskHandle_t astronomical_task_handle = NULL;

// Forward declarations
static void weather_task(void *pvParameters);
static void forecast_task(void *pvParameters);
static void astronomical_task(void *pvParameters);
static bool should_update_weather_data(void);
static bool should_update_forecast_data(void);
static bool should_update_astronomical_data(void);
static bool parse_weather_response(const char *response);
static bool parse_forecast_response(const char *response);
static bool parse_astronomical_response(const char *response);
static time_t parse_time_string(const char *time_str, struct tm *date);
static void update_astronomical_next_event_times(void);
static void check_midnight_rollover(void);
static void wait_for_time_sync(void);
static void format_time_based_on_wifi_format(time_t timestamp, char *buffer, size_t buffer_size);
static bool fetch_weather_data_internal(void);
static bool fetch_forecast_data_internal(void);
static bool fetch_astronomical_data_internal(void);
static time_t calculate_next_scheduled_time(int hour, int minute);
static time_t calculate_next_aligned_time(time_t base_time);
static void parse_time_config(const char *time_str, int *hour, int *minute);
static bool is_clock_time_format_24h(void);
static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt);
static esp_err_t forecast_http_event_handler(esp_http_client_event_t *evt);
static esp_err_t astronomical_http_event_handler(esp_http_client_event_t *evt);
static void log_memory_state(const char *location);

// Response buffers - reduced sizes to save memory
static char weather_response_buffer[4096];
static char forecast_response_buffer[4096];
static char astronomical_response_buffer[4096];

/**
 * @brief Log memory state for debugging
 */
static void log_memory_state(const char *location) {
    size_t free_heap = get_free_heap_size();
    ESP_LOGI(TAG, "Memory [%s] - Free heap: %u bytes", location, free_heap);
    if (free_heap < 30000) {
        ESP_LOGW(TAG, "Low memory warning at %s: %u bytes", location, free_heap);
    }
}

/**
 * @brief Parse time string from config (HH:MM)
 */
static void parse_time_config(const char *time_str, int *hour, int *minute) {
    if (!time_str || !hour || !minute) return;
    
    if (sscanf(time_str, "%d:%d", hour, minute) != 2) {
        *hour = 3;
        *minute = 0;
    }
}

/**
 * @brief Calculate next aligned update time based on a base time
 */
static time_t calculate_next_aligned_time(time_t base_time) {
    struct tm timeinfo;
    localtime_r(&base_time, &timeinfo);
    
    int interval = weather_config.weather_interval_minutes;
    
    struct tm next_tm = timeinfo;
    next_tm.tm_min += interval;
    next_tm.tm_sec = 0;
    
    if (next_tm.tm_min >= 60) {
        next_tm.tm_min -= 60;
        next_tm.tm_hour += 1;
    }
    
    if (next_tm.tm_hour >= 24) {
        next_tm.tm_hour -= 24;
        next_tm.tm_mday += 1;
    }
    
    return mktime(&next_tm);
}

/**
 * @brief Calculate next scheduled time for daily updates
 */
static time_t calculate_next_scheduled_time(int hour, int minute) {
    time_t now = time(NULL);
    struct tm timeinfo;
    
    if (now == 0 || now == -1) {
        return 0;
    }
    
    localtime_r(&now, &timeinfo);
    
    struct tm scheduled_tm = timeinfo;
    scheduled_tm.tm_hour = hour;
    scheduled_tm.tm_min = minute;
    scheduled_tm.tm_sec = 0;
    
    time_t scheduled_today = mktime(&scheduled_tm);
    
    if (scheduled_today <= now) {
        scheduled_tm.tm_mday += 1;
        scheduled_today = mktime(&scheduled_tm);
    }
    
    return scheduled_today;
}

/**
 * @brief Initialize weather configuration
 */
static void init_weather_config(void) {
    weather_config.latitude = atof(CONFIG_WEATHER_LATITUDE);
    weather_config.longitude = atof(CONFIG_WEATHER_LONGITUDE);
    
    strlcpy(weather_config.timezone, CONFIG_WEATHER_TIMEZONE, sizeof(weather_config.timezone));
    strlcpy(weather_config.api_key, CONFIG_ASTRONOMICAL_API_KEY, sizeof(weather_config.api_key));
    strlcpy(weather_config.city, CONFIG_ASTRONOMICAL_CITY, sizeof(weather_config.city));
    strlcpy(weather_config.astro_timezone, CONFIG_ASTRONOMICAL_TIMEZONE, sizeof(weather_config.astro_timezone));
    
    weather_config.enabled = CONFIG_WEATHER_ENABLE;
    weather_config.astro_enabled = weather_config.enabled && (weather_config.api_key[0] != '\0');
    weather_config.weather_interval_minutes = CONFIG_WEATHER_UPDATE_INTERVAL;
    
    parse_time_config(CONFIG_FORECAST_UPDATE_TIME,
                     &weather_config.forecast_update_hour,
                     &weather_config.forecast_update_minute);
    parse_time_config(CONFIG_ASTRONOMICAL_UPDATE_TIME,
                     &weather_config.astronomical_update_hour,
                     &weather_config.astronomical_update_minute);
    
    ESP_LOGI(TAG, "Weather config: enabled=%d, astro_enabled=%d", 
             weather_config.enabled, weather_config.astro_enabled);
    ESP_LOGI(TAG, "Schedule: Weather every %d min", weather_config.weather_interval_minutes);
    log_memory_state("after config init");
}

/**
 * @brief Find weather mapping by code
 */
static const weather_map_t* find_weather_map(uint8_t code) {
    for (size_t i = 0; i < sizeof(weather_map)/sizeof(weather_map[0]); i++) {
        if (weather_map[i].code == code) {
            return &weather_map[i];
        }
    }
    return NULL;
}

/**
 * @brief Check if wifi_clock is using 24h format
 */
static bool is_clock_time_format_24h(void) {
    wifi_clock_time_data_t time_data;
    if (wifi_clock_get_time_data(&time_data) == ESP_OK) {
        return time_data.is_24h_format;
    }
    return true;
}

/**
 * @brief Format time based on wifi format
 */
static void format_time_based_on_wifi_format(time_t timestamp, char *buffer, size_t buffer_size) {
    if (timestamp == 0 || !buffer || buffer_size < 6) {
        if (buffer && buffer_size > 0) {
            buffer[0] = '-';
            buffer[1] = '-';
            buffer[2] = ':';
            buffer[3] = '-';
            buffer[4] = '-';
            buffer[5] = '\0';
        }
        return;
    }
    
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    
    bool use_24h = is_clock_time_format_24h();
    
    if (use_24h) {
        snprintf(buffer, buffer_size, "%02d:%02d", 
                 timeinfo.tm_hour, timeinfo.tm_min);
    } else {
        int hour_12 = timeinfo.tm_hour % 12;
        if (hour_12 == 0) hour_12 = 12;
        
        const char *ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
        snprintf(buffer, buffer_size, "%02d:%02d %s", 
                 hour_12, timeinfo.tm_min, ampm);
    }
}

/**
 * @brief Wait for time sync (non-blocking)
 */
static void wait_for_time_sync(void) {
    static int retry_count = 0;
    const int max_retries = 10;
    
    if (!wifi_clock_is_connected()) {
        ESP_LOGI(TAG, "Wi-Fi not connected, waiting...");
        return;
    }

    if (!waiting_for_time_sync) return;
    
    if (wifi_clock_is_time_synced()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        time_t now = time(NULL);
        if (now > 1000000000) {
            
            char posix_tz[64];
            convert_to_posix_timezone(weather_config.timezone, posix_tz, sizeof(posix_tz));
            
            ESP_LOGI(TAG, "Setting timezone to: %s (from config: %s)", 
                     posix_tz, weather_config.timezone);
            
            setenv("TZ", posix_tz, 1);
            tzset();
            
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            
            waiting_for_time_sync = false;
            retry_count = 0;
            
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            int interval = weather_config.weather_interval_minutes;
            struct tm aligned_tm = timeinfo;
            
            int base_minute = 1;
            int aligned_minute;
            
            int minutes_since_base = (timeinfo.tm_min - base_minute + 60) % 60;
            int intervals_passed = minutes_since_base / interval;
            int remainder = minutes_since_base % interval;
            
            if (remainder > 0 || (timeinfo.tm_sec > 0 && remainder == 0)) {
                intervals_passed++;
            }
            
            aligned_minute = base_minute + (intervals_passed * interval);
            
            if (aligned_minute >= 60) {
                aligned_minute -= 60;
                aligned_tm.tm_hour++;
                if (aligned_tm.tm_hour >= 24) {
                    aligned_tm.tm_hour -= 24;
                    aligned_tm.tm_mday++;
                }
            }
            
            aligned_tm.tm_min = aligned_minute;
            aligned_tm.tm_sec = 0;
            
            last_weather_interval_update = mktime(&aligned_tm);
            
            next_scheduled_forecast_update = calculate_next_scheduled_time(
                weather_config.forecast_update_hour, weather_config.forecast_update_minute);
            next_scheduled_astronomical_update = calculate_next_scheduled_time(
                weather_config.astronomical_update_hour, weather_config.astronomical_update_minute);
            
            ESP_LOGI(TAG, "Time synced successfully");
            ESP_LOGI(TAG, "Boot time: %02d:%02d:%02d", 
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            ESP_LOGI(TAG, "Aligned to: %02d:%02d:00 (starting at minute %d)", 
                     aligned_tm.tm_hour, aligned_tm.tm_min, base_minute);
            
            log_memory_state("after time sync");
        } else {
            if (retry_count++ < max_retries) {
                ESP_LOGI(TAG, "Time synced but invalid, retrying... %d/%d", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                ESP_LOGW(TAG, "Max retries reached for time validation");
                waiting_for_time_sync = false;
            }
        }
    } else {
        ESP_LOGI(TAG, "Waiting for time sync...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Initialize weather module
 */
void weather_init(void) {
    if (is_initialized) return;
    
    ESP_LOGI(TAG, "Initializing weather module");
    log_memory_state("weather_init start");
    
    init_weather_config();
    
    memset(&weather_data, 0, sizeof(weather_data_t));
    memset(&astronomical_data, 0, sizeof(astronomical_data_t));
    memset(&weather_forecast, 0, sizeof(weather_forecast_t));
    
    weather_data.enabled = weather_config.enabled;
    astronomical_data.enabled = weather_config.astro_enabled;
    weather_forecast.enabled = weather_config.enabled;
    
    if (!weather_config.enabled) {
        ESP_LOGI(TAG, "Weather module disabled in config");
        is_initialized = true;
        return;
    }
    
    weather_mutex = xSemaphoreCreateMutex();
    if (!weather_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    is_initialized = true;

    ESP_LOGI(TAG, "Creating weather tasks...");
    
#if CONFIG_WEATHER_TASK_CORE_ID == -1
    xTaskCreate(weather_task, "weather_task", WEATHER_TASK_STACK, NULL, 
                CONFIG_WEATHER_TASK_PRIORITY, &weather_task_handle);
    xTaskCreate(forecast_task, "forecast_task", WEATHER_TASK_STACK, NULL, 
                CONFIG_WEATHER_TASK_PRIORITY + 1, &forecast_task_handle);
    if (weather_config.astro_enabled) {
        xTaskCreate(astronomical_task, "astro_task", WEATHER_TASK_STACK + 1024, NULL, 
                    CONFIG_WEATHER_TASK_PRIORITY + 2, &astronomical_task_handle);
    }
#else
    xTaskCreatePinnedToCore(weather_task, "weather_task", WEATHER_TASK_STACK, NULL,
                           CONFIG_WEATHER_TASK_PRIORITY, &weather_task_handle, CONFIG_WEATHER_TASK_CORE_ID);
    xTaskCreatePinnedToCore(forecast_task, "forecast_task", WEATHER_TASK_STACK, NULL,
                           CONFIG_WEATHER_TASK_PRIORITY + 1, &forecast_task_handle, CONFIG_WEATHER_TASK_CORE_ID);
    if (weather_config.astro_enabled) {
        xTaskCreatePinnedToCore(astronomical_task, "astro_task", WEATHER_TASK_STACK + 1024, NULL,
                               CONFIG_WEATHER_TASK_PRIORITY + 2, &astronomical_task_handle, CONFIG_WEATHER_TASK_CORE_ID);
    }
#endif
    
    ESP_LOGI(TAG, "Weather module initialized successfully");
    log_memory_state("weather_init end");
}

/**
 * @brief Get current weather data
 */
bool weather_get_data(weather_data_t *data) {
    if (!is_initialized || !data) {
        if (data) data->enabled = weather_config.enabled;
        return false;
    }
    
    if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(data, &weather_data, sizeof(weather_data_t));
        xSemaphoreGive(weather_mutex);
        return data->enabled && data->valid;
    }
    return false;
}

/**
 * @brief Get astronomical data
 */
bool weather_get_astronomical_data(astronomical_data_t *data) {
    if (!is_initialized || !data) {
        if (data) {
            data->enabled = astronomical_data.enabled;
            strlcpy(data->last_update, "--:--:--", sizeof(data->last_update));
        }
        return false;
    }
    
    if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        check_midnight_rollover();
        memcpy(data, &astronomical_data, sizeof(astronomical_data_t));
        xSemaphoreGive(weather_mutex);
        return data->enabled && data->valid;
    }
    return false;
}

/**
 * @brief Get forecast data
 */
bool weather_get_forecast(weather_forecast_t *forecast) {
    if (!is_initialized || !forecast) {
        if (forecast) forecast->enabled = weather_config.enabled;
        return false;
    }
    
    if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(forecast, &weather_forecast, sizeof(weather_forecast_t));
        xSemaphoreGive(weather_mutex);
        return forecast->enabled && forecast->valid;
    }
    return false;
}

/**
 * @brief Check for midnight rollover and trigger astronomical update
 */
static void check_midnight_rollover(void) {
    if (!astronomical_data.valid) return;
    
    time_t now = time(NULL);
    if (now == 0 || now == -1) return;
    
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_mday != last_check_day) {
        last_check_day = timeinfo.tm_mday;
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "MIDNIGHT DETECTED! Forcing astronomical API update");
        ESP_LOGI(TAG, "========================================");
        
        if (weather_config.astro_enabled) {
            if (astronomical_task_handle) {
                xTaskNotify(astronomical_task_handle, 1, eSetBits);
                ESP_LOGI(TAG, "Astronomical update triggered at midnight");
            } else {
                fetch_astronomical_data_internal();
            }
        }
        
        if (astronomical_data.next_sunrise > 0) {
            astronomical_data.sunrise = astronomical_data.next_sunrise;
            astronomical_data.next_sunrise = astronomical_data.sunrise + 86400;
        }
        if (astronomical_data.next_sunset > 0) {
            astronomical_data.sunset = astronomical_data.next_sunset;
            astronomical_data.next_sunset = astronomical_data.sunset + 86400;
        }
        if (astronomical_data.next_moonrise > 0) {
            astronomical_data.moonrise = astronomical_data.next_moonrise;
            astronomical_data.next_moonrise = astronomical_data.moonrise + 86400;
        }
        if (astronomical_data.next_moonset > 0) {
            astronomical_data.moonset = astronomical_data.next_moonset;
            astronomical_data.next_moonset = astronomical_data.moonset + 86400;
        }
        
        strftime(astronomical_data.last_update, sizeof(astronomical_data.last_update), 
                "%H:%M:%S", &timeinfo);
    }
}

/**
 * @brief Public API functions
 */
bool weather_update_in_progress(void) { return weather_update_in_progress_flag && weather_config.enabled; }
bool weather_forecast_update_in_progress(void) { return forecast_update_in_progress_flag && weather_config.enabled; }
bool weather_astronomical_update_in_progress(void) { return astronomical_update_in_progress_flag && astronomical_data.enabled; }
bool weather_is_enabled(void) { return weather_config.enabled; }
bool weather_astronomical_is_enabled(void) { return astronomical_data.enabled; }

void weather_force_update(void) { 
    if (is_initialized && weather_config.enabled) {
        ESP_LOGI(TAG, "Force update requested");
        if (weather_task_handle) {
            xTaskNotify(weather_task_handle, 1, eSetBits);
        }
    }
}

void weather_force_forecast_update(void) { 
    if (is_initialized && weather_config.enabled) {
        ESP_LOGI(TAG, "Force forecast update requested");
        if (forecast_task_handle) {
            xTaskNotify(forecast_task_handle, 1, eSetBits);
        }
    }
}

void weather_force_astronomical_update(void) { 
    if (is_initialized && astronomical_data.enabled) {
        ESP_LOGI(TAG, "Force astronomical update requested");
        if (astronomical_task_handle) {
            xTaskNotify(astronomical_task_handle, 1, eSetBits);
        }
    }
}

/**
 * @brief Format astronomical time for display
 */
bool weather_format_astronomical_time_display(time_t timestamp, time_t next_timestamp, 
                                              char *buffer, size_t buffer_size, bool *show_next) {
    if (!buffer) return false;
    
    if (show_next) *show_next = false;
    
    if (!astronomical_data.enabled || timestamp == 0) {
        if (buffer_size >= 6) {
            if (is_clock_time_format_24h()) {
                strlcpy(buffer, "--:--", buffer_size);
            } else {
                if (buffer_size >= 9) {
                    strlcpy(buffer, "--:-- --", buffer_size);
                } else {
                    strlcpy(buffer, "--:--", buffer_size);
                }
            }
        }
        return false;
    }
    
    time_t now = time(NULL);
    
    if (timestamp > 0 && timestamp < now && next_timestamp > 0) {
        format_time_based_on_wifi_format(next_timestamp, buffer, buffer_size);
        if (show_next) *show_next = true;
        return true;
    }
    
    if (timestamp > 0) {
        format_time_based_on_wifi_format(timestamp, buffer, buffer_size);
        return true;
    }
    
    if (buffer_size >= 6) {
        if (is_clock_time_format_24h()) {
            strlcpy(buffer, "--:--", buffer_size);
        } else {
            if (buffer_size >= 9) {
                strlcpy(buffer, "--:-- --", buffer_size);
            } else {
                strlcpy(buffer, "--:--", buffer_size);
            }
        }
    }
    return false;
}

const char* weather_get_description(int code) {
    const weather_map_t* mapping = find_weather_map(code);
    return mapping ? mapping->description : "Unknown";
}

const char* weather_get_icon(int code, bool is_day) {
    const weather_map_t* mapping = find_weather_map(code);
    return mapping ? (is_day ? mapping->icon_day : mapping->icon_night) : (is_day ? "01d" : "01n");
}

const char* weather_get_wind_direction(double degrees) {
    if (degrees >= 337.5 || degrees < 22.5) return "N";
    if (degrees < 67.5) return "NE";
    if (degrees < 112.5) return "E";
    if (degrees < 157.5) return "SE";
    if (degrees < 202.5) return "S";
    if (degrees < 247.5) return "SW";
    if (degrees < 292.5) return "W";
    return "NW";
}

const char* weather_get_moon_icon(float phase) {
    if (phase < 0.0f || phase > 1.0f) return moon_phase_icons[0];
    
    if (phase < 0.03f || phase > 0.97f) return moon_phase_icons[0];
    if (phase < 0.22f) return moon_phase_icons[1];
    if (phase < 0.28f) return moon_phase_icons[2];
    if (phase < 0.47f) return moon_phase_icons[3];
    if (phase < 0.53f) return moon_phase_icons[4];
    if (phase < 0.72f) return moon_phase_icons[5];
    if (phase < 0.78f) return moon_phase_icons[6];
    return moon_phase_icons[7];
}

const char* weather_get_moon_phase_name(float phase) {
    if (phase < 0.0f || phase > 1.0f) return "--";
    
    if (phase < 0.03f || phase > 0.97f) return moon_phase_names[0];
    if (phase < 0.22f) return moon_phase_names[1];
    if (phase < 0.28f) return moon_phase_names[2];
    if (phase < 0.47f) return moon_phase_names[3];
    if (phase < 0.53f) return moon_phase_names[4];
    if (phase < 0.72f) return moon_phase_names[5];
    if (phase < 0.78f) return moon_phase_names[6];
    return moon_phase_names[7];
}

/**
 * @brief Update astronomical next event times
 */
static void update_astronomical_next_event_times(void) {
    if (!astronomical_data.valid) return;
    
    time_t now = time(NULL);
    const time_t day_sec = 86400;
    
    if (astronomical_data.sunrise > 0) {
        astronomical_data.next_sunrise = (astronomical_data.sunrise < now) 
            ? astronomical_data.sunrise + day_sec : astronomical_data.sunrise;
    }
    if (astronomical_data.sunset > 0) {
        astronomical_data.next_sunset = (astronomical_data.sunset < now) 
            ? astronomical_data.sunset + day_sec : astronomical_data.sunset;
    }
    if (astronomical_data.moonrise > 0) {
        astronomical_data.next_moonrise = (astronomical_data.moonrise < now) 
            ? astronomical_data.moonrise + day_sec : astronomical_data.moonrise;
    }
    if (astronomical_data.moonset > 0) {
        astronomical_data.next_moonset = (astronomical_data.moonset < now) 
            ? astronomical_data.moonset + day_sec : astronomical_data.moonset;
    }
}

/**
 * @brief Check if we should update current weather data (aligned to intervals)
 */
static bool should_update_weather_data(void) {
    if (!weather_config.enabled) {
        return false;
    }
    
    if (!wifi_clock_is_connected()) {
        return false;
    }
    
    if (waiting_for_time_sync) {
        wait_for_time_sync();
        if (waiting_for_time_sync) {
            return false;
        }
    }
    
    time_t now = time(NULL);
    
    if (last_weather_interval_update == 0 || !weather_data.valid) {
        ESP_LOGI(TAG, "Initial weather update (immediate)");
        return true;
    }
    
    time_t next_aligned = calculate_next_aligned_time(last_weather_interval_update);
    
    if (now >= next_aligned) {
        ESP_LOGI(TAG, "Weather update triggered at aligned time for %d-min interval", 
                 weather_config.weather_interval_minutes);
        return true;
    }
    
    return false;
}

/**
 * @brief Check if we should update forecast data
 */
static bool should_update_forecast_data(void) {
    if (!weather_config.enabled) {
        return false;
    }
    
    if (!wifi_clock_is_connected()) {
        return false;
    }
    
    if (waiting_for_time_sync) {
        wait_for_time_sync();
        if (waiting_for_time_sync) {
            return false;
        }
    }
    
    time_t now = time(NULL);
    
    if (next_scheduled_forecast_update > 0 && now >= next_scheduled_forecast_update) {
        ESP_LOGI(TAG, "Scheduled daily forecast update triggered");
        next_scheduled_forecast_update = calculate_next_scheduled_time(
            weather_config.forecast_update_hour, weather_config.forecast_update_minute);
        last_forecast_update = now;
        return true;
    }
    
    if (last_forecast_update == 0 || !weather_forecast.valid) {
        last_forecast_update = now;
        return true;
    }
    
    return false;
}

/**
 * @brief Check if we should update astronomical data
 */
static bool should_update_astronomical_data(void) {
    if (!weather_config.astro_enabled) {
        return false;
    }
    
    if (!wifi_clock_is_connected()) {
        return false;
    }
    
    if (waiting_for_time_sync) {
        wait_for_time_sync();
        if (waiting_for_time_sync) {
            return false;
        }
    }
    
    time_t now = time(NULL);
    
    if (next_scheduled_astronomical_update > 0 && now >= next_scheduled_astronomical_update) {
        ESP_LOGI(TAG, "Scheduled daily astronomical update triggered");
        next_scheduled_astronomical_update = calculate_next_scheduled_time(
            weather_config.astronomical_update_hour, weather_config.astronomical_update_minute);
        last_astronomical_update = now;
        return true;
    }
    
    if (last_astronomical_update == 0 || !astronomical_data.valid) {
        last_astronomical_update = now;
        return true;
    }
    
    return false;
}

/**
 * @brief Parse time string from API
 */
static time_t parse_time_string(const char *time_str, struct tm *date) {
    if (!time_str || !date || time_str[0] == '\0') return 0;
    
    int hour, minute, second;
    if (sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) == 3) {
        struct tm timeinfo = *date;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;
        time_t result = mktime(&timeinfo);
        if (result == (time_t)-1) {
            ESP_LOGW(TAG, "Failed to convert time string: %s", time_str);
            return 0;
        }
        return result;
    }
    
    if (sscanf(time_str, "%d:%d", &hour, &minute) == 2) {
        struct tm timeinfo = *date;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = 0;
        time_t result = mktime(&timeinfo);
        if (result == (time_t)-1) {
            ESP_LOGW(TAG, "Failed to convert time string: %s", time_str);
            return 0;
        }
        return result;
    }
    
    ESP_LOGW(TAG, "Invalid time format: %s", time_str);
    return 0;
}

/**
 * @brief Parse current weather JSON response
 */
static bool parse_weather_response(const char *response) {
    if (!response) {
        ESP_LOGE(TAG, "No response to parse");
        return false;
    }
    
    // Log first 200 chars for debugging
    ESP_LOGD(TAG, "Response preview: %.200s", response);
    
    uint64_t parse_start = esp_timer_get_time() / 1000;
    
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }
    
    weather_data_t new_data = {0};
    new_data.enabled = weather_config.enabled;
    
    // Open-Meteo API v2 uses different structure
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (current) {
        // Get temperature
        cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
        if (temp && cJSON_IsNumber(temp)) {
            new_data.temperature = temp->valuedouble;
        }
        
        // Get weather code
        cJSON *code = cJSON_GetObjectItem(current, "weather_code");
        if (code && cJSON_IsNumber(code)) {
            new_data.weather_code = code->valueint;
        }
        
        // Get is_day
        cJSON *day = cJSON_GetObjectItem(current, "is_day");
        if (day && cJSON_IsNumber(day)) {
            new_data.is_day = (day->valueint != 0);
        }
        
        // Get humidity
        cJSON *humidity = cJSON_GetObjectItem(current, "relative_humidity_2m");
        if (humidity && cJSON_IsNumber(humidity)) {
            new_data.humidity = humidity->valuedouble;
        }
        
        // Get wind speed
        cJSON *wind_speed = cJSON_GetObjectItem(current, "wind_speed_10m");
        if (wind_speed && cJSON_IsNumber(wind_speed)) {
            new_data.wind_speed = wind_speed->valuedouble;
        }
        
        // Get wind direction
        cJSON *wind_dir = cJSON_GetObjectItem(current, "wind_direction_10m");
        if (wind_dir && cJSON_IsNumber(wind_dir)) {
            new_data.wind_direction = wind_dir->valuedouble;
        }
        
        // Get pressure
        cJSON *pressure = cJSON_GetObjectItem(current, "pressure_msl");
        if (pressure && cJSON_IsNumber(pressure)) {
            new_data.pressure = pressure->valuedouble;
        }
        
        // Get apparent temperature (feels like)
        cJSON *feels = cJSON_GetObjectItem(current, "apparent_temperature");
        if (feels && cJSON_IsNumber(feels)) {
            new_data.feels_like = feels->valuedouble;
        }
        
        // Get precipitation
        cJSON *precip = cJSON_GetObjectItem(current, "precipitation");
        if (precip && cJSON_IsNumber(precip)) {
            new_data.precipitation = precip->valuedouble;
        }
    }
    
    // Get daily data for min/max temps
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *temp_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
        if (temp_max && cJSON_IsArray(temp_max)) {
            cJSON *first_max = cJSON_GetArrayItem(temp_max, 0);
            if (first_max && cJSON_IsNumber(first_max)) {
                new_data.temp_max = first_max->valuedouble;
            }
        }
        
        cJSON *temp_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
        if (temp_min && cJSON_IsArray(temp_min)) {
            cJSON *first_min = cJSON_GetArrayItem(temp_min, 0);
            if (first_min && cJSON_IsNumber(first_min)) {
                new_data.temp_min = first_min->valuedouble;
            }
        }
    }
    
    // Get hourly data for precipitation probability
    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly) {
        cJSON *precip_prob = cJSON_GetObjectItem(hourly, "precipitation_probability");
        if (precip_prob && cJSON_IsArray(precip_prob)) {
            cJSON *first = cJSON_GetArrayItem(precip_prob, 0);
            if (first && cJSON_IsNumber(first)) {
                new_data.rain_probability = first->valuedouble;
            }
        }
    }
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(new_data.last_update, sizeof(new_data.last_update), "%H:%M:%S", &timeinfo);
    
    // Check if we got at least some valid data
    if (new_data.temperature != 0 || new_data.weather_code != 0) {
        new_data.valid = true;
        ESP_LOGI(TAG, "Weather updated: %.1f°C, code: %d", 
                 new_data.temperature, new_data.weather_code);
    } else {
        ESP_LOGE(TAG, "Failed to extract weather data from response");
    }
    
    cJSON_Delete(root);
    
    uint64_t parse_time = (esp_timer_get_time() / 1000) - parse_start;
    ESP_LOGI(TAG, "Weather JSON parsing took %llums", parse_time);
    
    if (new_data.valid) {
        if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&weather_data, &new_data, sizeof(weather_data_t));
            has_new_weather_data = true;
            weather_changed_flag = true;
            last_weather_change_time = time(NULL);
            xSemaphoreGive(weather_mutex);
            
            last_weather_update = time(NULL);
            weather_notify_weather_updated();
            return true;
        } else {
            ESP_LOGE(TAG, "Failed to take mutex for weather data update");
        }
    }
    
    return false;
}

/**
 * @brief Parse forecast JSON response
 */
static bool parse_forecast_response(const char *response) {
    if (!response) {
        ESP_LOGE(TAG, "No forecast response to parse");
        return false;
    }
    
    ESP_LOGD(TAG, "Forecast response preview: %.200s", response);
    
    uint64_t parse_start = esp_timer_get_time() / 1000;
    
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse forecast JSON");
        return false;
    }
    
    weather_forecast_t new_forecast = {0};
    new_forecast.enabled = weather_config.enabled;
    
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *dates = cJSON_GetObjectItem(daily, "time");
        cJSON *max_temps = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *min_temps = cJSON_GetObjectItem(daily, "temperature_2m_min");
        cJSON *weather_codes = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *precip_probs = cJSON_GetObjectItem(daily, "precipitation_probability_max");
        
        if (dates && max_temps && min_temps && weather_codes) {
            int forecast_days = cJSON_GetArraySize(dates);
            ESP_LOGI(TAG, "Forecast has %d days", forecast_days);
            
            int days_to_parse = (forecast_days > MAX_FORECAST_DAYS) ? MAX_FORECAST_DAYS : forecast_days;
            
            // Start from index 0 for today, or index 1 for tomorrow?
            // Let's use index 0 for simplicity
            for (int i = 0; i < days_to_parse; i++) {
                int day_index = i;
                
                // Parse date
                cJSON *date_json = cJSON_GetArrayItem(dates, i);
                if (date_json && cJSON_IsString(date_json)) {
                    int year, month, day;
                    if (sscanf(date_json->valuestring, "%d-%d-%d", &year, &month, &day) == 3) {
                        snprintf(new_forecast.days[day_index].date, 
                                sizeof(new_forecast.days[0].date), "%02d-%02d", day, month);
                    }
                }
                
                // Parse max temp
                cJSON *max_temp_json = cJSON_GetArrayItem(max_temps, i);
                if (max_temp_json && cJSON_IsNumber(max_temp_json)) {
                    new_forecast.days[day_index].temp_max = max_temp_json->valuedouble;
                }
                
                // Parse min temp
                cJSON *min_temp_json = cJSON_GetArrayItem(min_temps, i);
                if (min_temp_json && cJSON_IsNumber(min_temp_json)) {
                    new_forecast.days[day_index].temp_min = min_temp_json->valuedouble;
                }
                
                // Parse weather code
                cJSON *code_json = cJSON_GetArrayItem(weather_codes, i);
                if (code_json && cJSON_IsNumber(code_json)) {
                    new_forecast.days[day_index].weather_code = code_json->valueint;
                    // Assume day for forecast days
                    new_forecast.days[day_index].is_day = true;
                }
                
                // Parse precipitation probability
                if (precip_probs && cJSON_IsArray(precip_probs)) {
                    cJSON *precip_json = cJSON_GetArrayItem(precip_probs, i);
                    if (precip_json && cJSON_IsNumber(precip_json)) {
                        new_forecast.days[day_index].rain_probability = precip_json->valuedouble;
                    }
                }
                
                new_forecast.days[day_index].valid = true;
                ESP_LOGD(TAG, "Day %d: %s, max=%.1f, min=%.1f, code=%d", 
                         i, new_forecast.days[day_index].date,
                         new_forecast.days[day_index].temp_max,
                         new_forecast.days[day_index].temp_min,
                         new_forecast.days[day_index].weather_code);
            }
            
            new_forecast.num_days = days_to_parse;
            
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(new_forecast.last_update, sizeof(new_forecast.last_update), 
                    "%d-%b-%Y %H:%M:%S", &timeinfo);
            
            new_forecast.valid = true;
            
            ESP_LOGI(TAG, "Forecast updated: %d days", new_forecast.num_days);
        } else {
            ESP_LOGE(TAG, "Missing daily arrays in forecast response");
        }
    } else {
        ESP_LOGE(TAG, "No 'daily' object in forecast response");
    }
    
    cJSON_Delete(root);
    
    uint64_t parse_time = (esp_timer_get_time() / 1000) - parse_start;
    ESP_LOGI(TAG, "Forecast JSON parsing took %llums", parse_time);
    
    if (new_forecast.valid) {
        if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&weather_forecast, &new_forecast, sizeof(weather_forecast_t));
            has_new_forecast_data = true;
            forecast_changed_flag = true;
            last_forecast_change_time = time(NULL);
            xSemaphoreGive(weather_mutex);
            
            last_forecast_update = time(NULL);
            weather_notify_forecast_updated();
            return true;
        }
    } else {
        ESP_LOGE(TAG, "Forecast data not valid after parsing");
    }
    
    return false;
}

/**
 * @brief Parse astronomical JSON response
 */
static bool parse_astronomical_response(const char *response) {
    if (!response) {
        ESP_LOGE(TAG, "No astronomical response to parse");
        return false;
    }
    
    if (strstr(response, "Bad API Request") || 
        strstr(response, "Invalid location") ||
        strstr(response, "error") ||
        strlen(response) < 50) {
        ESP_LOGE(TAG, "API error response: %s", response);
        
        if (!waiting_for_time_sync) {
            next_scheduled_astronomical_update = time(NULL) + 300;
            ESP_LOGI(TAG, "Scheduled retry in 5 minutes");
        }
        return false;
    }
    
    uint64_t parse_start = esp_timer_get_time() / 1000;
    
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse astronomical JSON");
        return false;
    }
    
    astronomical_data_t new_data = {0};
    new_data.enabled = astronomical_data.enabled;
    
    cJSON *days = cJSON_GetObjectItem(root, "days");
    if (days && cJSON_IsArray(days) && cJSON_GetArraySize(days) >= 2) {
        cJSON *today = cJSON_GetArrayItem(days, 0);
        cJSON *tomorrow = cJSON_GetArrayItem(days, 1);
        
        if (today && tomorrow) {
            cJSON *date_today = cJSON_GetObjectItem(today, "datetime");
            cJSON *date_tomorrow = cJSON_GetObjectItem(tomorrow, "datetime");
            
            if (date_today && date_tomorrow) {
                struct tm today_tm = {0}, tomorrow_tm = {0};
                
                if (sscanf(date_today->valuestring, "%d-%d-%d",
                          &today_tm.tm_year, &today_tm.tm_mon, &today_tm.tm_mday) == 3 &&
                    sscanf(date_tomorrow->valuestring, "%d-%d-%d",
                          &tomorrow_tm.tm_year, &tomorrow_tm.tm_mon, &tomorrow_tm.tm_mday) == 3) {
                    
                    today_tm.tm_year -= 1900;
                    today_tm.tm_mon -= 1;
                    tomorrow_tm.tm_year -= 1900;
                    tomorrow_tm.tm_mon -= 1;
                    
                    cJSON *sunrise_today = cJSON_GetObjectItem(today, "sunrise");
                    cJSON *sunset_today = cJSON_GetObjectItem(today, "sunset");
                    cJSON *moonrise_today = cJSON_GetObjectItem(today, "moonrise");
                    cJSON *moonset_today = cJSON_GetObjectItem(today, "moonset");
                    cJSON *moonphase = cJSON_GetObjectItem(today, "moonphase");
                    
                    cJSON *sunrise_tomorrow = cJSON_GetObjectItem(tomorrow, "sunrise");
                    cJSON *sunset_tomorrow = cJSON_GetObjectItem(tomorrow, "sunset");
                    cJSON *moonrise_tomorrow = cJSON_GetObjectItem(tomorrow, "moonrise");
                    cJSON *moonset_tomorrow = cJSON_GetObjectItem(tomorrow, "moonset");
                    
                    if (sunrise_today && cJSON_IsString(sunrise_today)) {
                        new_data.sunrise = parse_time_string(sunrise_today->valuestring, &today_tm);
                    }
                    if (sunset_today && cJSON_IsString(sunset_today)) {
                        new_data.sunset = parse_time_string(sunset_today->valuestring, &today_tm);
                    }
                    if (moonrise_today && cJSON_IsString(moonrise_today) && moonrise_today->valuestring[0]) {
                        new_data.moonrise = parse_time_string(moonrise_today->valuestring, &today_tm);
                    }
                    if (moonset_today && cJSON_IsString(moonset_today) && moonset_today->valuestring[0]) {
                        new_data.moonset = parse_time_string(moonset_today->valuestring, &today_tm);
                    }
                    if (moonphase && cJSON_IsNumber(moonphase)) {
                        new_data.moon_phase = (float)moonphase->valuedouble;
                    }
                    
                    if (sunrise_tomorrow && cJSON_IsString(sunrise_tomorrow)) {
                        new_data.next_sunrise = parse_time_string(sunrise_tomorrow->valuestring, &tomorrow_tm);
                    }
                    if (sunset_tomorrow && cJSON_IsString(sunset_tomorrow)) {
                        new_data.next_sunset = parse_time_string(sunset_tomorrow->valuestring, &tomorrow_tm);
                    }
                    if (moonrise_tomorrow && cJSON_IsString(moonrise_tomorrow) && moonrise_tomorrow->valuestring[0]) {
                        new_data.next_moonrise = parse_time_string(moonrise_tomorrow->valuestring, &tomorrow_tm);
                    }
                    if (moonset_tomorrow && cJSON_IsString(moonset_tomorrow) && moonset_tomorrow->valuestring[0]) {
                        new_data.next_moonset = parse_time_string(moonset_tomorrow->valuestring, &tomorrow_tm);
                    }
                    
                    time_t now;
                    struct tm timeinfo;
                    time(&now);
                    localtime_r(&now, &timeinfo);
                    strftime(new_data.last_update, sizeof(new_data.last_update), "%H:%M:%S", &timeinfo);
                    
                    last_check_day = timeinfo.tm_mday;
                    new_data.valid = true;
                    
                    update_astronomical_next_event_times();
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    uint64_t parse_time = (esp_timer_get_time() / 1000) - parse_start;
    ESP_LOGI(TAG, "Astronomical JSON parsing took %llums", parse_time);
    
    if (new_data.valid) {
        if (xSemaphoreTake(weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&astronomical_data, &new_data, sizeof(astronomical_data_t));
            has_new_astro_data = true;
            astro_changed_flag = true;
            last_astro_change_time = time(NULL);
            xSemaphoreGive(weather_mutex);
            
            last_astronomical_update = time(NULL);
            weather_notify_astro_updated();
            return true;
        }
    }
    
    return false;
}

/**
 * @brief HTTP event handler for weather
 */
static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt) {
    static size_t response_len = 0;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!evt->data) break;
            if (response_len + evt->data_len < sizeof(weather_response_buffer)) {
                memcpy(weather_response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            } else {
                ESP_LOGE(TAG, "Weather response buffer overflow");
                response_len = 0;
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            if (response_len > 0) {
                weather_response_buffer[response_len] = '\0';
                parse_weather_response(weather_response_buffer);
            }
            response_len = 0;
            break;
            
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "Weather HTTP error");
            response_len = 0;
            weather_update_in_progress_flag = false;
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief HTTP event handler for forecast
 */
static esp_err_t forecast_http_event_handler(esp_http_client_event_t *evt) {
    static size_t response_len = 0;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!evt->data) break;
            if (response_len + evt->data_len < sizeof(forecast_response_buffer)) {
                memcpy(forecast_response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            } else {
                ESP_LOGE(TAG, "Forecast response buffer overflow");
                response_len = 0;
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            if (response_len > 0) {
                forecast_response_buffer[response_len] = '\0';
                parse_forecast_response(forecast_response_buffer);
            }
            response_len = 0;
            break;
            
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "Forecast HTTP error");
            response_len = 0;
            forecast_update_in_progress_flag = false;
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief HTTP event handler for astronomical data
 */
static esp_err_t astronomical_http_event_handler(esp_http_client_event_t *evt) {
    static size_t response_len = 0;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!evt->data) break;
            if (response_len + evt->data_len < sizeof(astronomical_response_buffer)) {
                memcpy(astronomical_response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            } else {
                ESP_LOGE(TAG, "Astronomical response buffer overflow");
                response_len = 0;
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            if (response_len > 0) {
                astronomical_response_buffer[response_len] = '\0';
                parse_astronomical_response(astronomical_response_buffer);
            }
            response_len = 0;
            break;
            
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "Astronomical HTTP error");
            response_len = 0;
            astronomical_update_in_progress_flag = false;
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Create HTTP client for weather
 */
static esp_http_client_handle_t create_weather_client(void) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?"
             "latitude=%.6f&longitude=%.6f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
             "is_day,precipitation,weather_code,pressure_msl,wind_speed_10m,wind_direction_10m"
             "&daily=temperature_2m_max,temperature_2m_min,weather_code"
             "&hourly=precipitation_probability"
             "&timezone=%s&forecast_days=2",
             weather_config.latitude, weather_config.longitude, weather_config.timezone);
    
    ESP_LOGD(TAG, "Weather URL: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = weather_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        ESP_LOGD(TAG, "Weather client created");
    }
    return client;
}

/**
 * @brief Create HTTP client for forecast
 */
static esp_http_client_handle_t create_forecast_client(void) {
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?"
             "latitude=%.6f&longitude=%.6f"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=%s&forecast_days=6",
             weather_config.latitude, weather_config.longitude, weather_config.timezone);
    
    ESP_LOGD(TAG, "Forecast URL: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = forecast_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        ESP_LOGD(TAG, "Forecast client created");
    }
    return client;
}

/**
 * @brief Create HTTP client for astronomical data
 */
static esp_http_client_handle_t create_astro_client(void) {
    char url[512];
    char today_str[11], tomorrow_str[11];
    
    time_t now;
    struct tm today, tomorrow;
    time(&now);
    localtime_r(&now, &today);
    
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", &today);
    
    time_t tomorrow_time = now + 86400;
    localtime_r(&tomorrow_time, &tomorrow);
    strftime(tomorrow_str, sizeof(tomorrow_str), "%Y-%m-%d", &tomorrow);
    
    char encoded_city[128];
    char *ptr = encoded_city;
    for (int i = 0; weather_config.city[i] && ptr < encoded_city + sizeof(encoded_city) - 1; i++) {
        if (weather_config.city[i] == ' ') {
            *ptr++ = '%';
            *ptr++ = '2';
            *ptr++ = '0';
        } else if (weather_config.city[i] == ',') {
            *ptr++ = '%';
            *ptr++ = '2';
            *ptr++ = 'C';
        } else {
            *ptr++ = weather_config.city[i];
        }
    }
    *ptr = '\0';
    
    snprintf(url, sizeof(url),
             "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/timeline/%s/%s/%s?"
             "unitGroup=metric&key=%s&include=days&elements=datetime,moonphase,sunrise,sunset,moonrise,moonset",
             encoded_city, today_str, tomorrow_str, weather_config.api_key);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = astronomical_http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        ESP_LOGD(TAG, "Astro client created");
    }
    return client;
}

/**
 * @brief Fetch current weather data with optimizations
 */
static bool fetch_weather_data_internal(void) {
    if (!weather_config.enabled) {
        ESP_LOGE(TAG, "Weather not enabled");
        return false;
    }

    if (!wifi_clock_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi disconnected");
        return false;
    }
    
    time_t now_sec = time(NULL);
    if (now_sec - last_retry_time < 5) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    last_retry_time = now_sec;
    
    log_memory_state("before weather fetch");
    
    uint64_t start_time = esp_timer_get_time() / 1000;
    
    esp_http_client_handle_t client = create_weather_client();
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client for weather");
        return false;
    }
    
    memset(weather_response_buffer, 0, sizeof(weather_response_buffer));
    
    weather_update_in_progress_flag = true;
    esp_err_t err = esp_http_client_perform(client);
    weather_update_in_progress_flag = false;
    
    esp_http_client_cleanup(client);
    
    uint64_t total_time = (esp_timer_get_time() / 1000) - start_time;
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Weather HTTP request failed after %llums: %s", 
                 total_time, esp_err_to_name(err));
        
        if (weather_retry_count < MAX_RETRIES) {
            weather_retry_count++;
            int delay = (1 << weather_retry_count) * 2000;
            ESP_LOGI(TAG, "Retrying weather fetch in %d ms (attempt %d/%d)", 
                     delay, weather_retry_count, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(delay));
            return fetch_weather_data_internal();
        }
        weather_retry_count = 0;
        return false;
    }
    
    weather_retry_count = 0;
    
    int status = esp_http_client_get_status_code(client);
    
    if (status == 200) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        int interval = weather_config.weather_interval_minutes;
        int base_minute = 1;
        
        int minutes_since_base = (timeinfo.tm_min - base_minute + 60) % 60;
        int current_interval = minutes_since_base / interval;
        
        struct tm aligned_tm = timeinfo;
        aligned_tm.tm_min = base_minute + (current_interval * interval);
        
        if (aligned_tm.tm_min >= 60) {
            aligned_tm.tm_min -= 60;
            aligned_tm.tm_hour++;
            if (aligned_tm.tm_hour >= 24) {
                aligned_tm.tm_hour -= 24;
                aligned_tm.tm_mday++;
            }
        }
        aligned_tm.tm_sec = 0;
        time_t aligned_time = mktime(&aligned_tm);
        
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Weather update completed at: %02d:%02d:%02d", 
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        ESP_LOGI(TAG, "This update corresponds to: %02d:%02d:00", 
                aligned_tm.tm_hour, aligned_tm.tm_min);
        ESP_LOGI(TAG, "Fetch took: %llums", total_time);
        
        last_weather_update = now;
        last_weather_interval_update = aligned_time;
        
        time_t next_aligned = calculate_next_aligned_time(aligned_time);
        
        struct tm next_tm;
        localtime_r(&next_aligned, &next_tm);
        
        ESP_LOGI(TAG, "Next update scheduled for: %02d:%02d:%02d", 
                next_tm.tm_hour, next_tm.tm_min, next_tm.tm_sec);
        ESP_LOGI(TAG, "========================================");
    }
    
    ESP_LOGI(TAG, "Weather fetch completed in %llums - Status: %d", 
             total_time, status);
    
    log_memory_state("after weather fetch");
    
    return (err == ESP_OK);
}

/**
 * @brief Fetch forecast data
 */
static bool fetch_forecast_data_internal(void) {
    if (!weather_config.enabled) {
        ESP_LOGE(TAG, "Weather not enabled");
        return false;
    }

    if (!wifi_clock_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi disconnected");
        return false;
    }
    
    time_t now_sec = time(NULL);
    if (now_sec - last_retry_time < 5) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    last_retry_time = now_sec;
    
    log_memory_state("before forecast fetch");
    
    uint64_t start_time = esp_timer_get_time() / 1000;
    
    esp_http_client_handle_t client = create_forecast_client();
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client for forecast");
        return false;
    }
    
    memset(forecast_response_buffer, 0, sizeof(forecast_response_buffer));
    
    forecast_update_in_progress_flag = true;
    esp_err_t err = esp_http_client_perform(client);
    forecast_update_in_progress_flag = false;
    
    esp_http_client_cleanup(client);
    
    uint64_t total_time = (esp_timer_get_time() / 1000) - start_time;
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Forecast HTTP request failed after %llums: %s", 
                 total_time, esp_err_to_name(err));
        
        if (forecast_retry_count < MAX_RETRIES) {
            forecast_retry_count++;
            int delay = (1 << forecast_retry_count) * 2000;
            ESP_LOGI(TAG, "Retrying forecast fetch in %d ms (attempt %d/%d)", 
                     delay, forecast_retry_count, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(delay));
            return fetch_forecast_data_internal();
        }
        forecast_retry_count = 0;
        return false;
    }
    
    forecast_retry_count = 0;
    
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Forecast fetch completed in %llums - Status: %d", 
             total_time, status);
    
    log_memory_state("after forecast fetch");
    
    return (err == ESP_OK);
}

/**
 * @brief Fetch astronomical data
 */
static bool fetch_astronomical_data_internal(void) {
    if (!weather_config.astro_enabled) {
        ESP_LOGE(TAG, "Astronomical data not enabled");
        return false;
    }

    if (!wifi_clock_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi disconnected");
        return false;
    }
    
    time_t now_sec = time(NULL);
    if (now_sec - last_retry_time < 5) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    last_retry_time = now_sec;
    
    log_memory_state("before astro fetch");
    
    uint64_t start_time = esp_timer_get_time() / 1000;
    
    esp_http_client_handle_t client = create_astro_client();
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client for astronomical data");
        return false;
    }
    
    memset(astronomical_response_buffer, 0, sizeof(astronomical_response_buffer));
    
    astronomical_update_in_progress_flag = true;
    esp_err_t err = esp_http_client_perform(client);
    astronomical_update_in_progress_flag = false;
    
    esp_http_client_cleanup(client);
    
    uint64_t total_time = (esp_timer_get_time() / 1000) - start_time;
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Astronomical HTTP request failed after %llums: %s", 
                 total_time, esp_err_to_name(err));
        
        if (astro_retry_count < MAX_RETRIES) {
            astro_retry_count++;
            int delay = (1 << astro_retry_count) * 2000;
            ESP_LOGI(TAG, "Retrying astronomical fetch in %d ms (attempt %d/%d)", 
                     delay, astro_retry_count, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(delay));
            return fetch_astronomical_data_internal();
        }
        astro_retry_count = 0;
        return false;
    }
    
    astro_retry_count = 0;
    
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Astronomical fetch completed in %llums - Status: %d", 
             total_time, status);
    
    log_memory_state("after astro fetch");
    
    return (err == ESP_OK);
}

/**
 * @brief Weather task
 */
static void weather_task(void *pvParameters) {
    ESP_LOGI(TAG, "Weather task started");
    log_memory_state("weather task start");
    
    uint32_t notification_value;
    
    while (1) {
        if (weather_config.enabled) {
            if (wifi_clock_is_connected()) {
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &notification_value, 0) == pdTRUE) {
                    ESP_LOGI(TAG, "Force update triggered for weather");
                    fetch_weather_data_internal();
                }
                
                if (should_update_weather_data()) {
                    fetch_weather_data_internal();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/**
 * @brief Forecast task
 */
static void forecast_task(void *pvParameters) {
    ESP_LOGI(TAG, "Forecast task started");
    log_memory_state("forecast task start");
    
    uint32_t notification_value;
    
    while (1) {
        if (weather_config.enabled) {
            if (wifi_clock_is_connected()) {
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &notification_value, 0) == pdTRUE) {
                    ESP_LOGI(TAG, "Force update triggered for forecast");
                    fetch_forecast_data_internal();
                }
                
                if (should_update_forecast_data()) {
                    fetch_forecast_data_internal();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/**
 * @brief Astronomical task
 */
static void astronomical_task(void *pvParameters) {
    ESP_LOGI(TAG, "Astronomical task started");
    log_memory_state("astro task start");
    
    while (waiting_for_time_sync) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Time synced, starting astronomical operations");
    
    uint32_t notification_value;
    int last_astro_day = -1;
    
    while (1) {
        if (weather_config.astro_enabled) {
            if (wifi_clock_is_connected()) {
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &notification_value, 0) == pdTRUE) {
                    ESP_LOGI(TAG, "Force update triggered for astronomical data");
                    fetch_astronomical_data_internal();
                }
                
                if (should_update_astronomical_data()) {
                    fetch_astronomical_data_internal();
                }
                
                time_t now = time(NULL);
                if (now > 1000000000) {
                    struct tm timeinfo;
                    localtime_r(&now, &timeinfo);
                    
                    if (timeinfo.tm_mday != last_astro_day) {
                        last_astro_day = timeinfo.tm_mday;
                        ESP_LOGI(TAG, "Midnight detected in task - forcing astronomical update");
                        fetch_astronomical_data_internal();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/**
 * @brief Get last weather update timestamp
 */
time_t weather_get_last_update_time(void) {
    return last_weather_update;
}

/**
 * @brief Get last forecast update timestamp
 */
time_t weather_get_last_forecast_update_time(void) {
    return last_forecast_update;
}

/**
 * @brief Get last astronomical update timestamp
 */
time_t weather_get_last_astronomical_update_time(void) {
    return last_astronomical_update;
}

/**
 * @brief Check if weather module is initialized
 */
bool weather_is_initialized(void) {
    return is_initialized;
}

/**
 * @brief Public API to fetch current weather data
 */
bool fetch_weather_data(void) {
    if (!weather_config.enabled) {
        ESP_LOGE(TAG, "Weather not enabled");
        return false;
    }
    return fetch_weather_data_internal();
}

/**
 * @brief Public API to fetch forecast data
 */
bool fetch_forecast_data(void) {
    if (!weather_config.enabled) {
        ESP_LOGE(TAG, "Weather not enabled");
        return false;
    }
    return fetch_forecast_data_internal();
}

/**
 * @brief Public API to fetch astronomical data
 */
bool fetch_astronomical_data(void) {
    if (!weather_config.astro_enabled) {
        ESP_LOGE(TAG, "Astronomical data not enabled");
        return false;
    }
    return fetch_astronomical_data_internal();
}

/**
 * @brief Set callback function to be called when new weather data is available
 */
void weather_set_update_callback(weather_update_callback_t callback) {
    update_callback = callback;
    ESP_LOGI(TAG, "Update callback %s", callback ? "set" : "cleared");
}

/**
 * @brief Notify that weather data has been updated
 */
void weather_notify_weather_updated(void) {
    ESP_LOGI(TAG, "Notifying weather data update");
    
    if (update_callback != NULL) {
        ESP_LOGI(TAG, "Calling update callback for weather");
        update_callback();
    }
}

/**
 * @brief Notify that forecast data has been updated
 */
void weather_notify_forecast_updated(void) {
    ESP_LOGI(TAG, "Notifying forecast data update");
    
    if (update_callback != NULL) {
        ESP_LOGI(TAG, "Calling update callback for forecast");
        update_callback();
    }
}

/**
 * @brief Notify that astronomical data has been updated
 */
void weather_notify_astro_updated(void) {
    ESP_LOGI(TAG, "Notifying astronomical data update");
    
    if (update_callback != NULL) {
        ESP_LOGI(TAG, "Calling update callback for astronomical");
        update_callback();
    }
}

/**
 * @brief Check if weather data has changed
 */
bool weather_is_weather_changed(void) {
    bool flag = weather_changed_flag;
    weather_changed_flag = false;
    return flag;
}

/**
 * @brief Check if forecast data has changed
 */
bool weather_is_forecast_changed(void) {
    bool flag = forecast_changed_flag;
    forecast_changed_flag = false;
    return flag;
}

/**
 * @brief Check if astronomical data has changed
 */
bool weather_is_astro_changed(void) {
    bool flag = astro_changed_flag;
    astro_changed_flag = false;
    return flag;
}

/**
 * @brief Get timestamp of last weather change
 */
time_t weather_get_last_weather_change_time(void) {
    return last_weather_change_time;
}

/**
 * @brief Get timestamp of last forecast change
 */
time_t weather_get_last_forecast_change_time(void) {
    return last_forecast_change_time;
}

/**
 * @brief Get timestamp of last astronomical change
 */
time_t weather_get_last_astro_change_time(void) {
    return last_astro_change_time;
}

/**
 * @brief Check if new weather data is available
 */
bool weather_has_new_data(void) {
    return has_new_weather_data || has_new_forecast_data || has_new_astro_data;
}

/**
 * @brief Clear new data flags after display update
 */
void weather_clear_new_data_flags(void) {
    has_new_weather_data = false;
    has_new_forecast_data = false;
    has_new_astro_data = false;
}

/**
 * @brief Check if new weather data is available
 */
bool weather_has_new_weather_data(void) {
    return has_new_weather_data;
}

/**
 * @brief Check if new forecast data is available
 */
bool weather_has_new_forecast_data(void) {
    return has_new_forecast_data;
}

/**
 * @brief Check if new astronomical data is available
 */
bool weather_has_new_astro_data(void) {
    return has_new_astro_data;
}

/**
 * @brief Log debug information about weather updates
 */
void weather_log_debug_info(void) {
    time_t weather_time = weather_get_last_update_time();
    time_t forecast_time = weather_get_last_forecast_update_time();
    time_t astro_time = weather_get_last_astronomical_update_time();
    
    char weather_str[32] = "Never";
    char forecast_str[32] = "Never";
    char astro_str[32] = "Never";
    
    if (weather_time > 0) {
        struct tm tm_info;
        localtime_r(&weather_time, &tm_info);
        strftime(weather_str, sizeof(weather_str), "%H:%M:%S", &tm_info);
    }
    
    if (forecast_time > 0) {
        struct tm tm_info;
        localtime_r(&forecast_time, &tm_info);
        strftime(forecast_str, sizeof(forecast_str), "%H:%M:%S", &tm_info);
    }
    
    if (astro_time > 0) {
        struct tm tm_info;
        localtime_r(&astro_time, &tm_info);
        strftime(astro_str, sizeof(astro_str), "%H:%M:%S", &tm_info);
    }
    
    ESP_LOGI(TAG, "Weather Debug - Last updates: Weather=%s, Forecast=%s, Astro=%s", 
             weather_str, forecast_str, astro_str);
    ESP_LOGI(TAG, "Weather Debug - Change flags: W=%d F=%d A=%d, New flags: W=%d F=%d A=%d", 
             weather_changed_flag, forecast_changed_flag, astro_changed_flag,
             has_new_weather_data, has_new_forecast_data, has_new_astro_data);
    log_memory_state("debug info");
    
    if (next_scheduled_forecast_update > 0) {
        struct tm scheduled;
        localtime_r(&next_scheduled_forecast_update, &scheduled);
        ESP_LOGI(TAG, "Next forecast scheduled: %02d:%02d on %02d/%02d", 
                 scheduled.tm_hour, scheduled.tm_min,
                 scheduled.tm_mday, scheduled.tm_mon + 1);
    }
    
    if (next_scheduled_astronomical_update > 0) {
        struct tm scheduled;
        localtime_r(&next_scheduled_astronomical_update, &scheduled);
        ESP_LOGI(TAG, "Next astronomical scheduled: %02d:%02d on %02d/%02d", 
                 scheduled.tm_hour, scheduled.tm_min,
                 scheduled.tm_mday, scheduled.tm_mon + 1);
    }
    
    if (last_weather_interval_update > 0) {
        int interval = weather_config.weather_interval_minutes;
        time_t next_aligned = calculate_next_aligned_time(last_weather_interval_update);
        struct tm next_tm;
        localtime_r(&next_aligned, &next_tm);
        ESP_LOGI(TAG, "Next weather aligned: %02d:%02d:%02d (every %d min)", 
                 next_tm.tm_hour, next_tm.tm_min, next_tm.tm_sec, interval);
    }
}
#else // CONFIG_WEATHER_ENABLE not defined

static const char *TAG = "WEATHER";
static volatile bool is_initialized = false;
static struct {
    bool enabled;
    bool astro_enabled;
} weather_config;

void weather_init(void) {
    ESP_LOGI(TAG, "Weather module disabled in config");
    weather_config.enabled = false;
    weather_config.astro_enabled = false;
    is_initialized = true;
}

bool weather_get_data(weather_data_t *data) {
    if (data) {
        memset(data, 0, sizeof(weather_data_t));
        data->enabled = false;
        strlcpy(data->last_update, "--:--:--", sizeof(data->last_update));
    }
    return false;
}

bool weather_get_astronomical_data(astronomical_data_t *data) {
    if (data) {
        memset(data, 0, sizeof(astronomical_data_t));
        data->enabled = false;
        strlcpy(data->last_update, "--:--:--", sizeof(data->last_update));
    }
    return false;
}

bool weather_get_forecast(weather_forecast_t *forecast) {
    if (forecast) {
        memset(forecast, 0, sizeof(weather_forecast_t));
        forecast->enabled = false;
        strlcpy(forecast->last_update, "--:--:--", sizeof(forecast->last_update));
    }
    return false;
}

bool weather_update_in_progress(void) { return false; }
bool weather_forecast_update_in_progress(void) { return false; }
bool weather_astronomical_update_in_progress(void) { return false; }
bool weather_is_enabled(void) { return false; }
bool weather_astronomical_is_enabled(void) { return false; }

void weather_force_update(void) { }
void weather_force_forecast_update(void) { }
void weather_force_astronomical_update(void) { }

bool weather_format_astronomical_time_display(time_t timestamp, time_t next_timestamp, 
                                              char *buffer, size_t buffer_size, bool *show_next) {
    if (buffer && buffer_size >= 6) {
        strlcpy(buffer, "--:--", buffer_size);
    }
    if (show_next) *show_next = false;
    return false;
}

const char* weather_get_description(int code) { return "Unknown"; }
const char* weather_get_icon(int code, bool is_day) { return is_day ? "01d" : "01n"; }
const char* weather_get_wind_direction(double degrees) { return "--"; }
const char* weather_get_moon_icon(float phase) { return "new_moon"; }
const char* weather_get_moon_phase_name(float phase) { return "--"; }

time_t weather_get_last_update_time(void) { return 0; }
time_t weather_get_last_forecast_update_time(void) { return 0; }
time_t weather_get_last_astronomical_update_time(void) { return 0; }
bool weather_is_initialized(void) { return is_initialized; }

bool fetch_weather_data(void) { return false; }
bool fetch_forecast_data(void) { return false; }
bool fetch_astronomical_data(void) { return false; }

void weather_set_update_callback(weather_update_callback_t callback) { }
void weather_notify_weather_updated(void) { }
void weather_notify_forecast_updated(void) { }
void weather_notify_astro_updated(void) { }
bool weather_is_weather_changed(void) { return false; }
bool weather_is_forecast_changed(void) { return false; }
bool weather_is_astro_changed(void) { return false; }
time_t weather_get_last_weather_change_time(void) { return 0; }
time_t weather_get_last_forecast_change_time(void) { return 0; }
time_t weather_get_last_astro_change_time(void) { return 0; }
bool weather_has_new_data(void) { return false; }
void weather_clear_new_data_flags(void) { }
bool weather_has_new_weather_data(void) { return false; }
bool weather_has_new_forecast_data(void) { return false; }
bool weather_has_new_astro_data(void) { return false; }
void weather_log_debug_info(void) { }

#endif // CONFIG_WEATHER_ENABLE