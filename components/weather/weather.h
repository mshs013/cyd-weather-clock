#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <time.h>
#include <stddef.h>  // Added for size_t

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of forecast days
#define MAX_FORECAST_DAYS 5

// ================= WEATHER DATA STRUCTURES =================

// Weather data structure
typedef struct {
    bool enabled;
    bool valid;
    char last_update[9];  // HH:MM:SS
    
    // Current conditions
    double temperature;       // °C
    double feels_like;        // °C
    double humidity;          // %
    double wind_speed;        // km/h
    double wind_direction;    // degrees
    double pressure;          // hPa
    int weather_code;         // WMO code
    bool is_day;              // true = day, false = night
    double precipitation;     // mm
    double rain_probability;  // %
    
    // Daily extremes
    double temp_max;          // °C
    double temp_min;          // °C
} weather_data_t;

// Forecast day structure
typedef struct {
    bool valid;
    char date[6];            // DD-MM
    double temp_max;         // °C
    double temp_min;         // °C
    int weather_code;        // WMO code
    bool is_day;             // always true for forecast
    double rain_probability; // %
} weather_day_t;

// Forecast data structure
typedef struct {
    bool enabled;
    bool valid;
    char last_update[22];    // DD-Mon-YYYY HH:MM:SS (increased from 9 to 20)
    int num_days;            // Number of valid forecast days
    weather_day_t days[MAX_FORECAST_DAYS];
} weather_forecast_t;

// Astronomical data structure
typedef struct {
    bool enabled;
    bool valid;
    char last_update[9];     // HH:MM:SS
    
    // Today's events
    time_t sunrise;          // Unix timestamp
    time_t sunset;           // Unix timestamp
    time_t moonrise;         // Unix timestamp
    time_t moonset;          // Unix timestamp
    float moon_phase;        // 0.0-1.0
    
    // Next day's events (if today's have passed)
    time_t next_sunrise;     // Unix timestamp
    time_t next_sunset;      // Unix timestamp
    time_t next_moonrise;    // Unix timestamp
    time_t next_moonset;     // Unix timestamp
} astronomical_data_t;

// ================= PUBLIC API =================

/**
 * @brief Initialize weather module (includes astronomical data)
 * 
 * This function initializes the weather module with configuration from Kconfig.
 * It creates separate tasks for weather, forecast, and astronomical updates.
 * The module requires time synchronization from wifi_clock before fetching data.
 */
void weather_init(void);

/**
 * @brief Get current weather data
 * @param data Pointer to weather_data_t structure to fill
 * @return true if data is valid and enabled, false otherwise
 */
bool weather_get_data(weather_data_t *data);

/**
 * @brief Get astronomical data
 * @param data Pointer to astronomical_data_t structure to fill
 * @return true if data is valid and enabled, false otherwise
 */
bool weather_get_astronomical_data(astronomical_data_t *data);

/**
 * @brief Get forecast data
 * @param forecast Pointer to weather_forecast_t structure to fill
 * @return true if forecast is valid and enabled, false otherwise
 */
bool weather_get_forecast(weather_forecast_t *forecast);

/**
 * @brief Check if weather update is in progress
 * @return true if update in progress, false otherwise
 */
bool weather_update_in_progress(void);

/**
 * @brief Check if forecast update is in progress
 * @return true if forecast update in progress, false otherwise
 */
bool weather_forecast_update_in_progress(void);

/**
 * @brief Check if astronomical update is in progress
 * @return true if astronomical update in progress, false otherwise
 */
bool weather_astronomical_update_in_progress(void);

/**
 * @brief Force immediate weather update
 * 
 * Triggers an immediate weather data fetch regardless of schedule.
 * Useful for manual refresh requests.
 */
void weather_force_update(void);

/**
 * @brief Force immediate forecast update
 * 
 * Triggers an immediate forecast data fetch regardless of schedule.
 * Useful for manual refresh requests.
 */
void weather_force_forecast_update(void);

/**
 * @brief Force immediate astronomical update
 * 
 * Triggers an immediate astronomical data fetch regardless of schedule.
 * Useful for manual refresh requests.
 */
void weather_force_astronomical_update(void);

/**
 * @brief Format astronomical time for display with (N) indicator
 * @param timestamp Today's event timestamp
 * @param next_timestamp Tomorrow's event timestamp
 * @param buffer Output buffer (minimum 6 bytes for "--:--")
 * @param buffer_size Size of output buffer
 * @param show_next Pointer to bool set to true if showing next day's event
 * @return true if successful, false otherwise
 */
bool weather_format_astronomical_time_display(time_t timestamp, time_t next_timestamp, 
                                              char *buffer, size_t buffer_size, bool *show_next);

/**
 * @brief Get weather description from WMO code
 * @param code WMO weather code
 * @return String description or "Unknown" if code not found
 */
const char* weather_get_description(int code);

/**
 * @brief Get weather icon from WMO code
 * @param code WMO weather code
 * @param is_day true for day icons, false for night icons
 * @return Icon code string (e.g., "01d", "10n") or default if code not found
 */
const char* weather_get_icon(int code, bool is_day);

/**
 * @brief Get wind direction from degrees
 * @param degrees Wind direction in degrees (0-360)
 * @return Compass direction string (N, NE, E, SE, S, SW, W, NW)
 */
const char* weather_get_wind_direction(double degrees);

/**
 * @brief Get moon phase icon name
 * @param phase Moon phase (0.0-1.0)
 * @return Moon phase icon name or default if phase invalid
 */
const char* weather_get_moon_icon(float phase);

/**
 * @brief Get moon phase display name
 * @param phase Moon phase (0.0-1.0)
 * @return Moon phase display name or "--" if phase invalid
 */
const char* weather_get_moon_phase_name(float phase);

/**
 * @brief Check if weather component is enabled in configuration
 * @return true if enabled, false otherwise
 */
bool weather_is_enabled(void);

/**
 * @brief Check if astronomical component is enabled in configuration
 * @return true if enabled, false otherwise
 */
bool weather_astronomical_is_enabled(void);

/**
 * @brief Fetch current weather data from Open-Meteo API
 * 
 * This function retrieves current weather data including temperature, humidity,
 * wind speed, precipitation, and other meteorological parameters. The data is
 * fetched based on configured latitude and longitude coordinates.
 * Updates occur:
 * 1. Every N minutes (configurable via WEATHER_UPDATE_INTERVAL)
 * 
 * @return true if data was successfully fetched and parsed, false otherwise
 * @note Requires time synchronization from wifi_clock before fetching
 */
bool fetch_weather_data(void);

/**
 * @brief Fetch 5-day weather forecast from Open-Meteo API
 * 
 * This function retrieves weather forecast data for the next 5 days including
 * daily high/low temperatures, weather conditions, and precipitation probability.
 * The forecast is updated once daily at the configured time (FORECAST_UPDATE_TIME).
 * 
 * @return true if forecast data was successfully fetched and parsed, false otherwise
 * @note Requires time synchronization from wifi_clock before fetching
 */
bool fetch_forecast_data(void);

/**
 * @brief Fetch astronomical data from Visual Crossing API
 * 
 * This function retrieves astronomical data including sunrise, sunset, moonrise,
 * moonset times and moon phase information. The data is fetched for the current
 * and next day to calculate upcoming astronomical events.
 * Updates occur once daily at configured time (ASTRONOMICAL_UPDATE_TIME).
 * 
 * @return true if astronomical data was successfully fetched and parsed, false otherwise
 * @note Requires time synchronization from wifi_clock and valid API key
 */
bool fetch_astronomical_data(void);

// ================= DEBUG/STATUS FUNCTIONS =================

/**
 * @brief Get last weather update timestamp
 * @return Unix timestamp of last successful weather update, 0 if never updated
 */
time_t weather_get_last_update_time(void);

/**
 * @brief Get last forecast update timestamp
 * @return Unix timestamp of last successful forecast update, 0 if never updated
 */
time_t weather_get_last_forecast_update_time(void);

/**
 * @brief Get last astronomical update timestamp
 * @return Unix timestamp of last successful astronomical update, 0 if never updated
 */
time_t weather_get_last_astronomical_update_time(void);

/**
 * @brief Check if weather module is initialized
 * @return true if initialized, false otherwise
 */
bool weather_is_initialized(void);

/**
 * @brief Callback type for weather update notifications
 */
typedef void (*weather_update_callback_t)(void);

/**
 * @brief Set callback function to be called when new weather data is available
 * @param callback Function pointer to callback, or NULL to disable
 */
void weather_set_update_callback(weather_update_callback_t callback);

/**
 * @brief Notify that weather data has been updated
 */
void weather_notify_weather_updated(void);

/**
 * @brief Notify that forecast data has been updated
 */
void weather_notify_forecast_updated(void);

/**
 * @brief Notify that astronomical data has been updated
 */
void weather_notify_astro_updated(void);

/**
 * @brief Check if weather data has changed since last check
 * @return true if weather data changed, false otherwise
 */
bool weather_is_weather_changed(void);

/**
 * @brief Check if forecast data has changed since last check
 * @return true if forecast data changed, false otherwise
 */
bool weather_is_forecast_changed(void);

/**
 * @brief Check if astronomical data has changed since last check
 * @return true if astronomical data changed, false otherwise
 */
bool weather_is_astro_changed(void);

/**
 * @brief Get timestamp of last weather change
 * @return Unix timestamp of last weather change
 */
time_t weather_get_last_weather_change_time(void);

/**
 * @brief Get timestamp of last forecast change
 * @return Unix timestamp of last forecast change
 */
time_t weather_get_last_forecast_change_time(void);

/**
 * @brief Get timestamp of last astronomical change
 * @return Unix timestamp of last astronomical change
 */
time_t weather_get_last_astro_change_time(void);

/**
 * @brief Check if new weather data is available (combined check)
 * @return true if any new data is available
 */
bool weather_has_new_data(void);

/**
 * @brief Clear all new data flags after display update
 */
void weather_clear_new_data_flags(void);

/**
 * @brief Check if new weather data is available (specific type)
 * @return true if new weather data is available
 */
bool weather_has_new_weather_data(void);

/**
 * @brief Check if new forecast data is available
 * @return true if new forecast data is available
 */
bool weather_has_new_forecast_data(void);

/**
 * @brief Check if new astronomical data is available
 * @return true if new astronomical data is available
 */
bool weather_has_new_astro_data(void);

/**
 * @brief Log debug information about weather updates
 */
void weather_log_debug_info(void);

#ifdef __cplusplus
}
#endif

#endif // WEATHER_H