#ifndef __WIFI_CLOCK_H__
#define __WIFI_CLOCK_H__

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi Clock status
 */
typedef enum {
    WIFI_CLOCK_STATUS_DISCONNECTED,    ///< Not connected to Wi-Fi
    WIFI_CLOCK_STATUS_CONNECTING,      ///< Connecting to Wi-Fi
    WIFI_CLOCK_STATUS_CONNECTED,       ///< Connected to Wi-Fi
    WIFI_CLOCK_STATUS_TIME_SYNCED,     ///< Time synchronized with NTP
    WIFI_CLOCK_STATUS_PROVISIONING,    ///< Provisioning mode active
    WIFI_CLOCK_STATUS_ERROR,           ///< Error state
} wifi_clock_status_t;

/**
 * @brief Time format options
 */
typedef enum {
    WIFI_CLOCK_TIME_FORMAT_24H,        ///< 24-hour format (HH:MM)
    WIFI_CLOCK_TIME_FORMAT_12H,        ///< 12-hour format (hh:MM AM/PM)
} wifi_clock_time_format_t;

/**
 * @brief Wi-Fi Clock events
 */
typedef enum {
    WIFI_CLOCK_EVENT_CONNECTED,        ///< Connected to Wi-Fi
    WIFI_CLOCK_EVENT_DISCONNECTED,     ///< Disconnected from Wi-Fi
    WIFI_CLOCK_EVENT_GOT_IP,           ///< Got IP address
    WIFI_CLOCK_EVENT_TIME_SYNCED,      ///< Time synchronized with NTP
    WIFI_CLOCK_EVENT_TIME_SYNC_FAILED, ///< Time synchronization failed
    WIFI_CLOCK_EVENT_CONNECTION_FAILED,///< Connection failed (after retries)
    WIFI_CLOCK_EVENT_PROVISIONING_STARTED, ///< Provisioning mode started
    WIFI_CLOCK_EVENT_PROVISIONING_STOPPED, ///< Provisioning mode stopped
    WIFI_CLOCK_EVENT_PROVISIONING_SUCCESS, ///< Provisioning successful, new credentials saved
    WIFI_CLOCK_EVENT_PROVISIONING_FAILED,  ///< Provisioning failed
} wifi_clock_event_t;

/**
 * @brief Provisioning method
 */
typedef enum {
    WIFI_CLOCK_PROV_SOFT_AP,           ///< Soft AP provisioning
} wifi_clock_prov_method_t;

/**
 * @brief Provisioning configuration
 */
typedef struct {
    wifi_clock_prov_method_t method;    ///< Provisioning method
    const char* service_name;           ///< Service name (AP SSID)
    const char* service_password;       ///< Service password (for Soft AP, NULL for open)
    uint8_t max_retries;                ///< Maximum provisioning retries
    uint32_t timeout_ms;                ///< Provisioning timeout in milliseconds
    bool reset_provisioned;              ///< Reset provisioned credentials before starting
} wifi_clock_prov_config_t;

/**
 * @brief Time data structure
 */
typedef struct {
    char time[6];                      ///< Formatted time (HH:MM or hh:MM)
    char seconds[3];                   ///< Seconds (SS)
    char date[32];                     ///< Formatted date (e.g., "Mon, 01-Jan-2024")
    char ampm[3];                      ///< AM/PM indicator (empty for 24h format)
    uint8_t week_number;               ///< Week number (1-52)
    bool is_synced;                    ///< Time is synchronized
    bool is_24h_format;                ///< Using 24-hour format
    time_t timestamp;                  ///< Unix timestamp
} wifi_clock_time_data_t;

/**
 * @brief Callback function type for Wi-Fi Clock events
 */
typedef void (*wifi_clock_callback_t)(wifi_clock_event_t event, void* arg);

/**
 * @brief Initialize Wi-Fi Clock component
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_FAIL: Initialization failed
 */
esp_err_t wifi_clock_init(void);

/**
 * @brief Deinitialize Wi-Fi Clock component
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 */
esp_err_t wifi_clock_deinit(void);

/**
 * @brief Start Wi-Fi connection and time synchronization
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t wifi_clock_start(void);

/**
 * @brief Stop Wi-Fi connection and time synchronization
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t wifi_clock_stop(void);

/**
 * @brief Start provisioning mode
 * 
 * @param config Provisioning configuration
 * @return esp_err_t 
 *         - ESP_OK: Provisioning started successfully
 *         - ESP_ERR_INVALID_STATE: Not initialized or already provisioning
 *         - ESP_ERR_INVALID_ARG: Invalid configuration
 *         - ESP_ERR_NOT_SUPPORTED: Provisioning method not supported
 */
esp_err_t wifi_clock_start_provisioning(const wifi_clock_prov_config_t *config);

/**
 * @brief Stop provisioning mode
 * 
 * @return esp_err_t 
 *         - ESP_OK: Provisioning stopped successfully
 *         - ESP_ERR_INVALID_STATE: Not in provisioning mode
 */
esp_err_t wifi_clock_stop_provisioning(void);

/**
 * @brief Check if in provisioning mode
 * 
 * @return true In provisioning mode
 * @return false Not in provisioning mode
 */
bool wifi_clock_is_provisioning(void);

/**
 * @brief Get current time data
 * 
 * @param time_data Pointer to time data structure
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_ARG: Invalid parameter
 *         - ESP_ERR_TIMEOUT: Timeout acquiring mutex
 */
esp_err_t wifi_clock_get_time_data(wifi_clock_time_data_t *time_data);

/**
 * @brief Manually trigger time synchronization
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 *         - ESP_ERR_NOT_SUPPORTED: Manual sync disabled
 */
esp_err_t wifi_clock_manual_sync(void);

/**
 * @brief Get current Wi-Fi Clock status
 * 
 * @return wifi_clock_status_t Current status
 */
wifi_clock_status_t wifi_clock_get_status(void);

/**
 * @brief Check if Wi-Fi is connected
 * 
 * @return true Wi-Fi is connected
 * @return false Wi-Fi is not connected
 */
bool wifi_clock_is_connected(void);

/**
 * @brief Check if time is synchronized
 * 
 * @return true Time is synchronized
 * @return false Time is not synchronized
 */
bool wifi_clock_is_time_synced(void);

/**
 * @brief Get component uptime in seconds
 * 
 * @return uint32_t Uptime in seconds
 */
uint32_t wifi_clock_get_uptime(void);

/**
 * @brief Get total connection retry count
 * 
 * @return uint32_t Total retry count
 */
uint32_t wifi_clock_get_retry_count(void);

/**
 * @brief Reset connection retry counter
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t wifi_clock_reset_retry_counter(void);

/**
 * @brief Register callback for Wi-Fi Clock events
 * 
 * @param callback Callback function
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_NOT_SUPPORTED: Callbacks disabled in config
 */
esp_err_t wifi_clock_register_callback(wifi_clock_callback_t callback);

/**
 * @brief Unregister callback for Wi-Fi Clock events
 * 
 * @return esp_err_t 
 *         - ESP_OK: Success
 */
esp_err_t wifi_clock_unregister_callback(void);

/**
 * @brief Enable or disable auto-reconnect
 * 
 * @param enable true to enable, false to disable
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t wifi_clock_set_auto_reconnect(bool enable);

/**
 * @brief Set timezone
 * 
 * @param timezone Timezone string (e.g., "UTC-6", "GMT+6", "Asia/Dhaka")
 * @return esp_err_t 
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Not initialized
 *         - ESP_ERR_INVALID_ARG: Invalid timezone
 */
esp_err_t wifi_clock_set_timezone(const char* timezone);

/**
 * @brief Get current timezone
 * 
 * @return const char* Current timezone
 */
const char* wifi_clock_get_timezone(void);

/**
 * @brief Convert timezone string to POSIX format
 * @param input Input timezone string (e.g., "Asia/Dhaka", "GMT+6")
 * @param output Output buffer for POSIX format
 * @param output_size Size of output buffer
 */
void convert_to_posix_timezone(const char* input, char* output, size_t output_size);

/**
 * @brief Get provisioned Wi-Fi credentials
 * 
 * @param ssid Buffer to store SSID (at least 32 bytes)
 * @param password Buffer to store password (at least 64 bytes)
 * @return esp_err_t 
 *         - ESP_OK: Credentials retrieved
 *         - ESP_FAIL: No credentials provisioned
 */
esp_err_t wifi_clock_get_provisioned_credentials(char* ssid, char* password);

/**
 * @brief Clear provisioned Wi-Fi credentials
 * 
 * @return esp_err_t 
 *         - ESP_OK: Credentials cleared
 *         - ESP_FAIL: Failed to clear credentials
 */
esp_err_t wifi_clock_clear_provisioned_credentials(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_CLOCK_H__ */