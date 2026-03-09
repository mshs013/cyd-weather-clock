#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "wifi_clock.h"

// Wi-Fi Provisioning includes
#include "esp_wifi_types.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

static const char *TAG = "wifi_clock";

// Configuration from menuconfig
#define WIFI_CLOCK_SSID CONFIG_WIFI_CLOCK_SSID
#define WIFI_CLOCK_PASSWORD CONFIG_WIFI_CLOCK_PASSWORD
#define WIFI_CLOCK_HOSTNAME CONFIG_WIFI_CLOCK_HOSTNAME
#define WIFI_CLOCK_TASK_STACK_SIZE CONFIG_WIFI_CLOCK_TASK_STACK_SIZE
#define WIFI_CLOCK_TASK_PRIORITY CONFIG_WIFI_CLOCK_TASK_PRIORITY
#define WIFI_CLOCK_TASK_CORE_ID CONFIG_WIFI_CLOCK_TASK_CORE_ID
#define WIFI_CLOCK_ENABLE_LOGGING CONFIG_WIFI_CLOCK_ENABLE_LOGGING
#define WIFI_CLOCK_POWER_SAVE_MODE CONFIG_WIFI_CLOCK_POWER_SAVE_MODE
#define WIFI_CLOCK_TIMEZONE CONFIG_WIFI_CLOCK_TIMEZONE
#define WIFI_CLOCK_NTP_SERVER CONFIG_WIFI_CLOCK_NTP_SERVER
#define WIFI_CLOCK_NTP_SERVER_2 CONFIG_WIFI_CLOCK_NTP_SERVER_2
#define WIFI_CLOCK_NTP_SERVER_3 CONFIG_WIFI_CLOCK_NTP_SERVER_3
#define WIFI_CLOCK_ENABLE_DAILY_SYNC CONFIG_WIFI_CLOCK_ENABLE_DAILY_SYNC
#define WIFI_CLOCK_DAILY_SYNC_HOUR CONFIG_WIFI_CLOCK_DAILY_SYNC_HOUR
#define WIFI_CLOCK_DAILY_SYNC_MINUTE CONFIG_WIFI_CLOCK_DAILY_SYNC_MINUTE
#define WIFI_CLOCK_SYNC_RETRY_COUNT CONFIG_WIFI_CLOCK_SYNC_RETRY_COUNT
#define WIFI_CLOCK_SYNC_TIMEOUT_MS CONFIG_WIFI_CLOCK_SYNC_TIMEOUT_MS
#define WIFI_CLOCK_SYNC_RETRY_INTERVAL_MS CONFIG_WIFI_CLOCK_SYNC_RETRY_INTERVAL_MS
#define WIFI_CLOCK_ENABLE_DEBUG CONFIG_WIFI_CLOCK_ENABLE_DEBUG
#define WIFI_CLOCK_TIME_UPDATE_INTERVAL_MS CONFIG_WIFI_CLOCK_TIME_UPDATE_INTERVAL_MS
#define WIFI_CLOCK_ENABLE_MANUAL_SYNC CONFIG_WIFI_CLOCK_ENABLE_MANUAL_SYNC
#define WIFI_CLOCK_EVENT_QUEUE_SIZE CONFIG_WIFI_CLOCK_EVENT_QUEUE_SIZE
#define WIFI_CLOCK_ENABLE_CALLBACKS CONFIG_WIFI_CLOCK_ENABLE_CALLBACKS
#define WIFI_CLOCK_AUTO_CONNECT CONFIG_WIFI_CLOCK_AUTO_CONNECT
#define WIFI_CLOCK_MUTEX_TIMEOUT_MS CONFIG_WIFI_CLOCK_MUTEX_TIMEOUT_MS

// Time format selection
#if CONFIG_WIFI_CLOCK_FORMAT_24H
    #define TIME_FORMAT WIFI_CLOCK_TIME_FORMAT_24H
#else
    #define TIME_FORMAT WIFI_CLOCK_TIME_FORMAT_12H
#endif

// Provisioning configuration defaults
#ifndef WIFI_CLOCK_PROV_SOFT_AP_MAX_RETRIES
#define WIFI_CLOCK_PROV_SOFT_AP_MAX_RETRIES 3
#endif

#ifndef WIFI_CLOCK_PROV_SOFT_AP_TIMEOUT_MS
#define WIFI_CLOCK_PROV_SOFT_AP_TIMEOUT_MS 300000  // 5 minutes
#endif

#ifndef WIFI_CLOCK_PROV_SOFT_AP_SSID
#define WIFI_CLOCK_PROV_SOFT_AP_SSID "WIFI_CLOCK_PROV"
#endif

#ifndef WIFI_CLOCK_PROV_SOFT_AP_PASSWORD
#define WIFI_CLOCK_PROV_SOFT_AP_PASSWORD ""
#endif

// NVS namespace for provisioning
#define WIFI_CLOCK_NVS_NAMESPACE "wifi_clock"
#define WIFI_CLOCK_NVS_SSID_KEY "ssid"
#define WIFI_CLOCK_NVS_PASSWORD_KEY "password"

// Internal events
typedef enum {
    INTERNAL_EVENT_START,
    INTERNAL_EVENT_STOP,
    INTERNAL_EVENT_RECONNECT,
    INTERNAL_EVENT_SYNC_TIME,
    INTERNAL_EVENT_DAILY_SYNC,
    INTERNAL_EVENT_WIFI_CONNECTED,
    INTERNAL_EVENT_WIFI_DISCONNECTED,
    INTERNAL_EVENT_IP_GOT,
    INTERNAL_EVENT_SNTP_SYNCED,
    INTERNAL_EVENT_SNTP_FAILED,
    INTERNAL_EVENT_CONNECTION_FAILED,
    INTERNAL_EVENT_PROVISIONING_START,
    INTERNAL_EVENT_PROVISIONING_STOP,
    INTERNAL_EVENT_PROVISIONING_SUCCESS,
    INTERNAL_EVENT_PROVISIONING_FAILED
} internal_event_t;

// Internal structure
typedef struct {
    TaskHandle_t task_handle;
    QueueHandle_t event_queue;
    SemaphoreHandle_t mutex;
    wifi_clock_status_t status;
    wifi_clock_callback_t user_callback;
    bool initialized;
    bool wifi_started;
    bool sntp_started;
    bool auto_reconnect;
    bool pending_reconnect;
    bool provisioning_active;
    TickType_t next_retry_time;
    uint32_t total_retry_count;
    uint32_t sync_retry_count;
    time_t last_sync_time;
    uint32_t start_time;
    char current_timezone[64];
    wifi_clock_prov_config_t prov_config;
    uint32_t prov_retry_count;
} wifi_clock_context_t;

static wifi_clock_context_t ctx = {0};

// Function prototypes
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
static void time_sync_notification_cb(struct timeval *tv);
static void wifi_clock_task(void *pvParameters);
static void sync_time_with_ntp(void);
static void format_time_data(wifi_clock_time_data_t *time_data);
static int get_week_number(struct tm *timeinfo);
static void start_wifi_connection(void);
static void stop_wifi_connection(void);
static void handle_wifi_connected(void);
static void handle_wifi_disconnected(void);
static void handle_ip_got(void);
static void handle_time_synced(void);
static void handle_time_sync_failed(void);
static void handle_connection_failed(void);
static void check_daily_sync(void);
static void connect_to_wifi(void);
static void schedule_reconnection(void);
static esp_err_t init_nvs(void);
static void reset_retry_counter(void);
static bool is_valid_timezone(const char* timezone);

// Provisioning function prototypes
static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data);
static void provisioning_start_softap(void);
static void provisioning_stop(void);
static esp_err_t save_provisioned_credentials(const char *ssid, const char *password);
static esp_err_t load_provisioned_credentials(char *ssid, size_t ssid_len, 
                                              char *password, size_t pass_len);
static void provisioning_cleanup(void);
static void reset_wifi_after_provisioning(void);

// Internal helper functions
static esp_err_t init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t wifi_clock_init(void) {
    if (ctx.initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    
    // Initialize context
    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
    ctx.initialized = false;
    ctx.wifi_started = false;
    ctx.sntp_started = false;
    ctx.auto_reconnect = WIFI_CLOCK_AUTO_CONNECT;
    ctx.pending_reconnect = false;
    ctx.provisioning_active = false;
    ctx.next_retry_time = 0;
    ctx.total_retry_count = 0;
    ctx.sync_retry_count = 0;
    ctx.last_sync_time = 0;
    ctx.start_time = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    ctx.prov_retry_count = 0;
    memset(&ctx.prov_config, 0, sizeof(ctx.prov_config));
    strncpy(ctx.current_timezone, WIFI_CLOCK_TIMEZONE, sizeof(ctx.current_timezone) - 1);
    ctx.current_timezone[sizeof(ctx.current_timezone) - 1] = '\0';
    
    // Create mutex
    ctx.mutex = xSemaphoreCreateMutex();
    if (ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // Create event queue
    ctx.event_queue = xQueueCreate(WIFI_CLOCK_EVENT_QUEUE_SIZE, sizeof(internal_event_t));
    if (ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        vSemaphoreDelete(ctx.mutex);
        return ESP_FAIL;
    }
    
    // Initialize NVS
    ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS");
        vQueueDelete(ctx.event_queue);
        vSemaphoreDelete(ctx.mutex);
        return ret;
    }
    
    // Initialize Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set power save mode
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_CLOCK_POWER_SAVE_MODE));
    
    // Register event handlers FIRST
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ip_event_handler,
                                                        NULL,
                                                        NULL));
    
    // Register provisioning event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, 
                                                ESP_EVENT_ANY_ID, 
                                                &provisioning_event_handler, 
                                                NULL));
    
    // Create Wi-Fi Clock task
    BaseType_t task_created = xTaskCreatePinnedToCore(
        wifi_clock_task,
        "wifi_clock_task",
        WIFI_CLOCK_TASK_STACK_SIZE,
        NULL,
        WIFI_CLOCK_TASK_PRIORITY,
        &ctx.task_handle,
        WIFI_CLOCK_TASK_CORE_ID
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi Clock task");
        vQueueDelete(ctx.event_queue);
        vSemaphoreDelete(ctx.mutex);
        return ESP_FAIL;
    }
    
    ctx.initialized = true;
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Wi-Fi Clock initialized successfully");
        ESP_LOGI(TAG, "Auto-reconnect: %s", ctx.auto_reconnect ? "Enabled" : "Disabled");
        ESP_LOGI(TAG, "Timezone: %s", ctx.current_timezone);
    }
    
    return ESP_OK;
}

esp_err_t wifi_clock_deinit(void) {
    if (!ctx.initialized) {
        return ESP_OK;
    }
    
    // Stop provisioning if active
    if (ctx.provisioning_active) {
        wifi_clock_stop_provisioning();
    }
    
    wifi_clock_stop();
    
    if (ctx.task_handle != NULL) {
        vTaskDelete(ctx.task_handle);
        ctx.task_handle = NULL;
    }
    
    if (ctx.event_queue != NULL) {
        vQueueDelete(ctx.event_queue);
        ctx.event_queue = NULL;
    }
    
    if (ctx.mutex != NULL) {
        vSemaphoreDelete(ctx.mutex);
        ctx.mutex = NULL;
    }
    
    // Unregister event handlers
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler);
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provisioning_event_handler);
    
    esp_wifi_deinit();
    
    ctx.initialized = false;
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Wi-Fi Clock deinitialized");
    }
    
    return ESP_OK;
}

esp_err_t wifi_clock_start(void) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    internal_event_t event = INTERNAL_EVENT_START;
    if (xQueueSend(ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send start event");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t wifi_clock_stop(void) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    internal_event_t event = INTERNAL_EVENT_STOP;
    if (xQueueSend(ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send stop event");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t wifi_clock_start_provisioning(const wifi_clock_prov_config_t *config) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    
    // Check if already provisioning
    if (ctx.provisioning_active) {
        xSemaphoreGive(ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Only support SoftAP provisioning
    if (config->method != WIFI_CLOCK_PROV_SOFT_AP) {
        ESP_LOGE(TAG, "Only SoftAP provisioning is supported");
        xSemaphoreGive(ctx.mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Copy provisioning config
    memcpy(&ctx.prov_config, config, sizeof(wifi_clock_prov_config_t));
    ctx.prov_retry_count = 0;
    
    xSemaphoreGive(ctx.mutex);
    
    // First, ensure any existing provisioning is cleaned up
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Send provisioning start event
    internal_event_t event = INTERNAL_EVENT_PROVISIONING_START;
    if (xQueueSend(ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send provisioning start event");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t wifi_clock_stop_provisioning(void) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    
    if (!ctx.provisioning_active) {
        xSemaphoreGive(ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreGive(ctx.mutex);
    
    internal_event_t event = INTERNAL_EVENT_PROVISIONING_STOP;
    if (xQueueSend(ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send provisioning stop event");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

bool wifi_clock_is_provisioning(void) {
    bool is_active;
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    is_active = ctx.provisioning_active;
    xSemaphoreGive(ctx.mutex);
    return is_active;
}

esp_err_t wifi_clock_get_time_data(wifi_clock_time_data_t *time_data) {
    if (!ctx.initialized || time_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(WIFI_CLOCK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    format_time_data(time_data);
    
    xSemaphoreGive(ctx.mutex);
    return ESP_OK;
}

esp_err_t wifi_clock_manual_sync(void) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!WIFI_CLOCK_ENABLE_MANUAL_SYNC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Don't allow manual sync during provisioning
    if (ctx.provisioning_active) {
        return ESP_ERR_INVALID_STATE;
    }
    
    internal_event_t event = INTERNAL_EVENT_SYNC_TIME;
    if (xQueueSend(ctx.event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send sync event");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

wifi_clock_status_t wifi_clock_get_status(void) {
    wifi_clock_status_t status;
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    status = ctx.status;
    xSemaphoreGive(ctx.mutex);
    return status;
}

esp_err_t wifi_clock_register_callback(wifi_clock_callback_t callback) {
    if (!WIFI_CLOCK_ENABLE_CALLBACKS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    ctx.user_callback = callback;
    return ESP_OK;
}

esp_err_t wifi_clock_unregister_callback(void) {
    ctx.user_callback = NULL;
    return ESP_OK;
}

bool wifi_clock_is_connected(void) {
    return (ctx.status == WIFI_CLOCK_STATUS_CONNECTED || 
            ctx.status == WIFI_CLOCK_STATUS_TIME_SYNCED);
}

bool wifi_clock_is_time_synced(void) {
    return (ctx.status == WIFI_CLOCK_STATUS_TIME_SYNCED);
}

uint32_t wifi_clock_get_uptime(void) {
    return (xTaskGetTickCount() * portTICK_PERIOD_MS / 1000) - ctx.start_time;
}

static void reset_retry_counter(void) {
    ctx.total_retry_count = 0;
    ctx.pending_reconnect = false;
}

uint32_t wifi_clock_get_retry_count(void) {
    return ctx.total_retry_count;
}

esp_err_t wifi_clock_set_auto_reconnect(bool enable) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.auto_reconnect = enable;
    
    // If disabling, clear any pending reconnect
    if (!enable) {
        ctx.pending_reconnect = false;
    }
    
    xSemaphoreGive(ctx.mutex);
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Auto-reconnect %s", enable ? "enabled" : "disabled");
    }
    return ESP_OK;
}

esp_err_t wifi_clock_set_timezone(const char* timezone) {
    if (!ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (timezone == NULL || strlen(timezone) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!is_valid_timezone(timezone)) {
        ESP_LOGE(TAG, "Invalid timezone format: %s", timezone);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    strncpy(ctx.current_timezone, timezone, sizeof(ctx.current_timezone) - 1);
    ctx.current_timezone[sizeof(ctx.current_timezone) - 1] = '\0';
    xSemaphoreGive(ctx.mutex);
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Timezone set to: %s", timezone);
    }
    
    return ESP_OK;
}

const char* wifi_clock_get_timezone(void) {
    return ctx.current_timezone;
}

esp_err_t wifi_clock_get_provisioned_credentials(char* ssid, char* password) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return load_provisioned_credentials(ssid, 32, password, 64);
}

esp_err_t wifi_clock_clear_provisioned_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, WIFI_CLOCK_NVS_SSID_KEY);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, WIFI_CLOCK_NVS_PASSWORD_KEY);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Provisioned credentials cleared");
    }
    
    return err;
}

// Internal task function
static void wifi_clock_task(void *pvParameters) {
    internal_event_t event;
    TickType_t last_wake_time = xTaskGetTickCount();
    
    while (1) {
        // Check for events with timeout
        TickType_t timeout = WIFI_CLOCK_TIME_UPDATE_INTERVAL_MS;
        
        // If we have a pending reconnect, adjust timeout
        if (ctx.pending_reconnect && !ctx.provisioning_active) {
            TickType_t now = xTaskGetTickCount();
            if (now >= ctx.next_retry_time) {
                // Time to reconnect
                internal_event_t reconnect_event = INTERNAL_EVENT_RECONNECT;
                xQueueSend(ctx.event_queue, &reconnect_event, 0);
                ctx.pending_reconnect = false;
            } else {
                // Calculate timeout until next retry
                TickType_t time_until_retry = ctx.next_retry_time - now;
                if (time_until_retry < timeout) {
                    timeout = time_until_retry;
                }
            }
        }
        
        if (xQueueReceive(ctx.event_queue, &event, pdMS_TO_TICKS(timeout)) == pdTRUE) {
            switch (event) {
                case INTERNAL_EVENT_START:
                    start_wifi_connection();
                    break;
                    
                case INTERNAL_EVENT_STOP:
                    stop_wifi_connection();
                    // Clear pending reconnect when explicitly stopped
                    ctx.pending_reconnect = false;
                    break;
                    
                case INTERNAL_EVENT_RECONNECT:
                    if (!ctx.provisioning_active) {
                        connect_to_wifi();
                    }
                    break;
                    
                case INTERNAL_EVENT_SYNC_TIME:
                    if (!ctx.provisioning_active) {
                        sync_time_with_ntp();
                    }
                    break;
                    
                case INTERNAL_EVENT_DAILY_SYNC:
                    if (WIFI_CLOCK_ENABLE_DAILY_SYNC && !ctx.provisioning_active) {
                        sync_time_with_ntp();
                    }
                    break;
                    
                case INTERNAL_EVENT_WIFI_CONNECTED:
                    if (!ctx.provisioning_active) {
                        handle_wifi_connected();
                        // Clear pending reconnect flag on successful connection
                        ctx.pending_reconnect = false;
                    }
                    break;
                    
                case INTERNAL_EVENT_WIFI_DISCONNECTED:
                    if (!ctx.provisioning_active) {
                        handle_wifi_disconnected();
                    }
                    break;
                    
                case INTERNAL_EVENT_IP_GOT:
                    if (!ctx.provisioning_active) {
                        handle_ip_got();
                    }
                    break;
                    
                case INTERNAL_EVENT_SNTP_SYNCED:
                    if (!ctx.provisioning_active) {
                        handle_time_synced();
                    }
                    break;
                    
                case INTERNAL_EVENT_SNTP_FAILED:
                    if (!ctx.provisioning_active) {
                        handle_time_sync_failed();
                    }
                    break;
                    
                case INTERNAL_EVENT_CONNECTION_FAILED:
                    if (!ctx.provisioning_active) {
                        handle_connection_failed();
                        // Clear pending reconnect flag on final failure
                        ctx.pending_reconnect = false;
                    }
                    break;
                    
                case INTERNAL_EVENT_PROVISIONING_START:
                    // Stop normal Wi-Fi operations
                    if (ctx.wifi_started) {
                        stop_wifi_connection();
                    }
                    
                    // Disable auto-reconnect during provisioning
                    ctx.auto_reconnect = false;
                    ctx.pending_reconnect = false;
                    
                    // Start SoftAP provisioning
                    provisioning_start_softap();
                    break;
                    
                case INTERNAL_EVENT_PROVISIONING_STOP:
                    provisioning_stop();
                    break;

                case INTERNAL_EVENT_PROVISIONING_SUCCESS:
                    ESP_LOGI(TAG, "PROVISIONING SUCCESS - starting normal WiFi connection");
                    
                    // Ensure provisioning state is completely cleared
                    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
                    ctx.provisioning_active = false;
                    ctx.prov_retry_count = 0;
                    ctx.auto_reconnect = WIFI_CLOCK_AUTO_CONNECT;
                    ctx.total_retry_count = 0;
                    xSemaphoreGive(ctx.mutex);
                    
                    // Send callback to UI
                    if (ctx.user_callback != NULL) {
                        ctx.user_callback(WIFI_CLOCK_EVENT_PROVISIONING_SUCCESS, NULL);
                    }
                    
                    // Reset WiFi for normal operation
                    reset_wifi_after_provisioning();
                    
                    // Start normal WiFi connection with saved credentials
                    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
                    start_wifi_connection();
                    break;
                    
                case INTERNAL_EVENT_PROVISIONING_FAILED:
                    provisioning_cleanup();
                    
                    // Restore auto-reconnect setting
                    ctx.auto_reconnect = WIFI_CLOCK_AUTO_CONNECT;
                    
                    if (ctx.user_callback != NULL) {
                        ctx.user_callback(WIFI_CLOCK_EVENT_PROVISIONING_FAILED, NULL);
                    }
                    break;
            }
        }
        
        // Check for daily sync
        if (!ctx.provisioning_active) {
            check_daily_sync();
        }
        
        // Maintain periodic task wakeup
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(100)); // Small delay for responsiveness
    }
    
    vTaskDelete(NULL);
}

static void start_wifi_connection(void) {
    if (ctx.wifi_started || ctx.provisioning_active) {
        return;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_CONNECTING;
    xSemaphoreGive(ctx.mutex);
    
    // Reset retry counter when starting fresh
    reset_retry_counter();
    
    // Check if we have provisioned credentials
    char ssid[32] = {0};
    char password[64] = {0};
    esp_err_t ret = load_provisioned_credentials(ssid, sizeof(ssid), password, sizeof(password));
    
    wifi_config_t wifi_config = {0};
    
    if (ret == ESP_OK && strlen(ssid) > 0) {
        // Use provisioned credentials
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Using provisioned credentials for SSID: %s", ssid);
    } else {
        // Use default credentials from menuconfig
        strncpy((char*)wifi_config.sta.ssid, WIFI_CLOCK_SSID, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, WIFI_CLOCK_PASSWORD, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "Using default credentials for SSID: %s", WIFI_CLOCK_SSID);
    }
    
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Set hostname before starting Wi-Fi
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        const char *hostname = WIFI_CLOCK_HOSTNAME;
        esp_err_t err = esp_netif_set_hostname(netif, hostname);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Hostname set to: %s", hostname);
        } else {
            ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to get network interface");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    
    ctx.wifi_started = true;
    
    // Start connection attempt after a short delay
    vTaskDelay(pdMS_TO_TICKS(100));
    connect_to_wifi();
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Wi-Fi started, connecting to SSID: %s", wifi_config.sta.ssid);
    }
}

static void connect_to_wifi(void) {
    if (!ctx.wifi_started || ctx.provisioning_active) {
        return;
    }
    
    ESP_LOGI(TAG, "Attempting to connect to Wi-Fi...");
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "WiFi already connecting - will retry later");
            // This is normal, schedule reconnection
            schedule_reconnection();
        } else {
            ESP_LOGE(TAG, "Failed to start Wi-Fi connection: %s", esp_err_to_name(ret));
            
            xSemaphoreTake(ctx.mutex, portMAX_DELAY);
            ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
            xSemaphoreGive(ctx.mutex);
            
            // Schedule reconnection
            schedule_reconnection();
        }
    }
}

static void stop_wifi_connection(void) {
    if (!ctx.wifi_started) {
        return;
    }
    
    // Disable auto-reconnect when explicitly stopping
    ctx.auto_reconnect = false;
    
    ESP_ERROR_CHECK(esp_wifi_stop());
    ctx.wifi_started = false;
    
    if (ctx.sntp_started) {
        esp_sntp_stop();
        ctx.sntp_started = false;
    }
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
    xSemaphoreGive(ctx.mutex);
    
    // Reset retry counter
    reset_retry_counter();
    ctx.pending_reconnect = false;
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Wi-Fi stopped");
    }
}

static void schedule_reconnection(void) {
    // Don't schedule reconnection if provisioning is active
    if (ctx.provisioning_active) {
        ctx.pending_reconnect = false;
        return;
    }
    
    // Always retry if we have provisioned credentials
    char ssid[32] = {0};
    char password[64] = {0};
    esp_err_t ret = load_provisioned_credentials(ssid, sizeof(ssid), password, sizeof(password));
    bool has_credentials = (ret == ESP_OK && strlen(ssid) > 0);
    
    if (ctx.auto_reconnect || has_credentials) {
        ESP_LOGW(TAG, "Scheduling reconnection (attempt %lu)", ctx.total_retry_count + 1);
        
        // Increment retry counter
        ctx.total_retry_count++;
        
        // Exponential backoff but max 30 seconds
        uint32_t delay_ms = 1000;  // Start with 1 second
        if (ctx.total_retry_count > 3) {
            delay_ms = 5000;  // After 3 retries, wait 5 seconds
        }
        if (ctx.total_retry_count > 6) {
            delay_ms = 10000;  // After 6 retries, wait 10 seconds
        }
        
        ctx.next_retry_time = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
        ctx.pending_reconnect = true;
        
        ESP_LOGI(TAG, "Next reconnection attempt in %lu ms", delay_ms);
    } else {
        ESP_LOGW(TAG, "Not retrying connection. No credentials and auto-reconnect disabled");
        
        // Notify connection failure
        internal_event_t event = INTERNAL_EVENT_CONNECTION_FAILED;
        xQueueSend(ctx.event_queue, &event, 0);
        
        // Reset pending reconnect flag
        ctx.pending_reconnect = false;
    }
}

static void sync_time_with_ntp(void) {
    if (!ctx.sntp_started) {
        // Initialize SNTP
        ESP_LOGI(TAG, "Initializing SNTP");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        
        // Set servers
        esp_sntp_setservername(0, WIFI_CLOCK_NTP_SERVER);
        esp_sntp_setservername(1, WIFI_CLOCK_NTP_SERVER_2);
        esp_sntp_setservername(2, WIFI_CLOCK_NTP_SERVER_3);

        char final_tz[64];
        convert_to_posix_timezone(ctx.current_timezone, final_tz, sizeof(final_tz));
        
        // Set timezone
        setenv("TZ", final_tz, 1);
        tzset();
        
        // Set sync notification callback
        esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        
        esp_sntp_init();
        ctx.sntp_started = true;
    }
    
    // Set sync interval (1 hour)
    esp_sntp_set_sync_interval(3600000);
    
    if (WIFI_CLOCK_ENABLE_LOGGING) {
        ESP_LOGI(TAG, "Waiting for system time to be set...");
    }
}

static void format_time_data(wifi_clock_time_data_t *time_data) {
    if (time_data == NULL) return;
    
    // Check if time was ever successfully synced
    time_t now;
    time(&now);
    
    // If system time is before year 2000 or after year 2100, it's not valid
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    // Check if time is valid (year > 2000 and year < 2100)
    bool time_is_valid = (timeinfo.tm_year >= 100 && timeinfo.tm_year < 200); // 2000-2099
    
    if (!time_is_valid) {
        // Return placeholder values when time is not valid (never synced or reset)
        strcpy(time_data->time, "--:--");
        strcpy(time_data->seconds, "--");
        strcpy(time_data->date, "---, -- --- ----");
        strcpy(time_data->ampm, "--");
        time_data->week_number = 0;
        time_data->is_synced = false;
        time_data->timestamp = 0;
        time_data->is_24h_format = (TIME_FORMAT == WIFI_CLOCK_TIME_FORMAT_24H);
        return;
    }
    
    // Format time - we have valid system time, regardless of current WiFi status
    if (TIME_FORMAT == WIFI_CLOCK_TIME_FORMAT_24H) {
        strftime(time_data->time, sizeof(time_data->time), "%H:%M", &timeinfo);
        time_data->ampm[0] = '\0';
        time_data->is_24h_format = true;
    } else {
        strftime(time_data->time, sizeof(time_data->time), "%I:%M", &timeinfo);
        strftime(time_data->ampm, sizeof(time_data->ampm), "%p", &timeinfo);
        time_data->is_24h_format = false;
    }
    
    // Format seconds
    strftime(time_data->seconds, sizeof(time_data->seconds), "%S", &timeinfo);
    
    // Format date
    strftime(time_data->date, sizeof(time_data->date), "%a, %d-%b-%Y", &timeinfo);
    
    // Get week number
    time_data->week_number = get_week_number(&timeinfo);
    
    // Set sync status - indicate if we have valid time
    time_data->is_synced = true;
    time_data->timestamp = now;
}

static int get_week_number(struct tm *timeinfo) {
    char week_str[3];
    strftime(week_str, sizeof(week_str), "%V", timeinfo);
    return atoi(week_str); // Convert to 1-based week number
}

static void check_daily_sync(void) {
    if (!WIFI_CLOCK_ENABLE_DAILY_SYNC || ctx.status != WIFI_CLOCK_STATUS_TIME_SYNCED) {
        return;
    }
    
    struct tm timeinfo;
    time_t now;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Check if it's time for daily sync
    if (timeinfo.tm_hour == WIFI_CLOCK_DAILY_SYNC_HOUR && 
        timeinfo.tm_min == WIFI_CLOCK_DAILY_SYNC_MINUTE &&
        timeinfo.tm_sec == 0) {
        
        // Check if we haven't synced in the last 23 hours
        if ((now - ctx.last_sync_time) > (23 * 3600)) {
            internal_event_t event = INTERNAL_EVENT_DAILY_SYNC;
            xQueueSend(ctx.event_queue, &event, 0);
        }
    }
}

// Event handlers
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Wi-Fi station started");
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                if (WIFI_CLOCK_ENABLE_LOGGING && !ctx.provisioning_active) {
                    wifi_event_sta_connected_t* connected = (wifi_event_sta_connected_t*) event_data;
                    ESP_LOGI(TAG, "Connected to AP: %s, channel: %d", 
                             connected->ssid, connected->channel);
                }
                internal_event_t event = INTERNAL_EVENT_WIFI_CONNECTED;
                xQueueSend(ctx.event_queue, &event, 0);
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                if (!ctx.provisioning_active) {
                    wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
                    ESP_LOGW(TAG, "Disconnected from AP. Reason: %d", disconnected->reason);
                }
                
                event = INTERNAL_EVENT_WIFI_DISCONNECTED;
                xQueueSend(ctx.event_queue, &event, 0);
                break;
                
            default:
                break;
        }
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        
        if (WIFI_CLOCK_ENABLE_LOGGING && !ctx.provisioning_active) {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        
        internal_event_t internal_event = INTERNAL_EVENT_IP_GOT;
        xQueueSend(ctx.event_queue, &internal_event, 0);
    }
}

static void time_sync_notification_cb(struct timeval *tv) {
    ctx.last_sync_time = tv->tv_sec;
    
    if (WIFI_CLOCK_ENABLE_LOGGING && !ctx.provisioning_active) {
        ESP_LOGI(TAG, "Time synchronized with NTP server");
    }
    
    internal_event_t event = INTERNAL_EVENT_SNTP_SYNCED;
    xQueueSend(ctx.event_queue, &event, 0);
}

static void handle_wifi_connected(void) {
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_CONNECTED;
    // Reset retry counter on successful connection
    ctx.total_retry_count = 0;
    ctx.pending_reconnect = false;
    xSemaphoreGive(ctx.mutex);
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_CONNECTED, NULL);
    }
}

static void handle_wifi_disconnected(void) {
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
    xSemaphoreGive(ctx.mutex);
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_DISCONNECTED, NULL);
    }
    
    // ALWAYS retry if we have provisioned credentials
    char ssid[32] = {0};
    char password[64] = {0};
    esp_err_t ret = load_provisioned_credentials(ssid, sizeof(ssid), password, sizeof(password));
    
    if (ret == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Have provisioned credentials for %s - will retry connection", ssid);
        
        // Force reconnect regardless of auto_reconnect setting
        ctx.auto_reconnect = true;
        ctx.total_retry_count = 0;  // Reset retry counter
        schedule_reconnection();
    } else {
        ESP_LOGW(TAG, "No provisioned credentials - not retrying");
    }
}

static void handle_ip_got(void) {
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_GOT_IP, NULL);
    }
    
    // Start time sync
    internal_event_t event = INTERNAL_EVENT_SYNC_TIME;
    xQueueSend(ctx.event_queue, &event, 0);
}

static void handle_time_synced(void) {
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_TIME_SYNCED;
    ctx.sync_retry_count = 0;
    xSemaphoreGive(ctx.mutex);
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_TIME_SYNCED, NULL);
    }
}

static void handle_time_sync_failed(void) {
    ctx.sync_retry_count++;
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_TIME_SYNC_FAILED, NULL);
    }
    
    if (ctx.sync_retry_count < WIFI_CLOCK_SYNC_RETRY_COUNT) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_CLOCK_SYNC_RETRY_INTERVAL_MS));
        internal_event_t event = INTERNAL_EVENT_SYNC_TIME;
        xQueueSend(ctx.event_queue, &event, 0);
    } else {
        ESP_LOGE(TAG, "Time sync failed after %d attempts", WIFI_CLOCK_SYNC_RETRY_COUNT);
    }
}

static void handle_connection_failed(void) {
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.status = WIFI_CLOCK_STATUS_ERROR;
    xSemaphoreGive(ctx.mutex);
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_CONNECTION_FAILED, NULL);
    }
}

/**
 * @brief Convert timezone string to POSIX format
 * @param input Input timezone string (e.g., "Asia/Dhaka", "GMT+6", "UTC+6")
 * @param output Output buffer for POSIX format
 * @param output_size Size of output buffer
 */
void convert_to_posix_timezone(const char* input, char* output, size_t output_size) {
    if (input == NULL || output == NULL || output_size == 0) {
        return;
    }
    
    // Default to input if no conversion needed
    strncpy(output, input, output_size - 1);
    output[output_size - 1] = '\0';
    
    // Handle common timezone formats
    if (strcmp(input, "Asia/Dhaka") == 0 || strcmp(input, "GMT+6") == 0) {
        strncpy(output, "UTC-6", output_size - 1);
        output[output_size - 1] = '\0';
        if (WIFI_CLOCK_ENABLE_DEBUG) {
            ESP_LOGI(TAG, "Converted %s to POSIX: %s", input, output);
        }
    } else if (strstr(input, "GMT+") != NULL) {
        int offset;
        if (sscanf(input, "GMT+%d", &offset) == 1) {
            snprintf(output, output_size, "UTC-%d", offset);
            if (WIFI_CLOCK_ENABLE_DEBUG) {
                ESP_LOGI(TAG, "Converted %s to POSIX: %s", input, output);
            }
        }
    } else if (strstr(input, "GMT-") != NULL) {
        int offset;
        if (sscanf(input, "GMT-%d", &offset) == 1) {
            snprintf(output, output_size, "UTC+%d", offset);
            if (WIFI_CLOCK_ENABLE_DEBUG) {
                ESP_LOGI(TAG, "Converted %s to POSIX: %s", input, output);
            }
        }
    } else if (strstr(input, "UTC+") != NULL) {
        int offset;
        if (sscanf(input, "UTC+%d", &offset) == 1) {
            snprintf(output, output_size, "UTC-%d", offset);
            if (WIFI_CLOCK_ENABLE_DEBUG) {
                ESP_LOGI(TAG, "Converted %s to POSIX: %s", input, output);
            }
        }
    } else if (strstr(input, "UTC-") != NULL) {
        int offset;
        if (sscanf(input, "UTC-%d", &offset) == 1) {
            snprintf(output, output_size, "UTC+%d", offset);
            if (WIFI_CLOCK_ENABLE_DEBUG) {
                ESP_LOGI(TAG, "Converted %s to POSIX: %s", input, output);
            }
        }
    }
}

static bool is_valid_timezone(const char* timezone) {
    if (timezone == NULL || strlen(timezone) == 0) {
        return false;
    }
    
    // Check if it's a valid timezone format
    // Accept common formats: UTC±HH, GMT±HH, or Olson timezone names
    if (strstr(timezone, "UTC+") == timezone ||
        strstr(timezone, "UTC-") == timezone ||
        strstr(timezone, "GMT+") == timezone ||
        strstr(timezone, "GMT-") == timezone) {
        return true;
    }
    
    // Accept common Olson timezone names (you can expand this list)
    const char* valid_olson_tz[] = {
        "Asia/Dhaka",
        "America/New_York",
        "America/Los_Angeles",
        "Europe/London",
        "Europe/Paris",
        "Asia/Tokyo",
        "Australia/Sydney",
        NULL
    };
    
    for (int i = 0; valid_olson_tz[i] != NULL; i++) {
        if (strcmp(timezone, valid_olson_tz[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

// ==================== PROVISIONING IMPLEMENTATION ====================

static void provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    static bool success_sent = false;
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                success_sent = false;
                xSemaphoreTake(ctx.mutex, portMAX_DELAY);
                ctx.provisioning_active = true;
                ctx.status = WIFI_CLOCK_STATUS_PROVISIONING;
                xSemaphoreGive(ctx.mutex);
                
                if (ctx.user_callback != NULL) {
                    ctx.user_callback(WIFI_CLOCK_EVENT_PROVISIONING_STARTED, NULL);
                }
                break;

            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received credentials for SSID: %s", (char *)wifi_sta_cfg->ssid);
                
                // Save credentials to NVS
                save_provisioned_credentials((char *)wifi_sta_cfg->ssid, 
                                            (char *)wifi_sta_cfg->password);
                break;
            }
                
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning CRED_SUCCESS - WiFi connected successfully");
                // Don't stop provisioning here - let it complete
                break;
                
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "Provisioning END received - cleaning up");
                
                // Now send success event
                if (!success_sent) {
                    // Longer delay to ensure phone receives confirmation
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    
                    // IMPORTANT: Clear provisioning state BEFORE sending success
                    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
                    ctx.provisioning_active = false;
                    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
                    xSemaphoreGive(ctx.mutex);
                    
                    // Deinit provisioning manager completely
                    wifi_prov_mgr_stop_provisioning();
                    wifi_prov_mgr_deinit();
                    
                    // Send success event
                    internal_event_t event = INTERNAL_EVENT_PROVISIONING_SUCCESS;
                    xQueueSend(ctx.event_queue, &event, 0);
                    success_sent = true;
                    
                    ESP_LOGI(TAG, "Provisioning complete - system ready for normal WiFi");
                }
                break;
                
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed. Reason: %d", *reason);
                
                ctx.prov_retry_count++;
                
                if (ctx.prov_retry_count < ctx.prov_config.max_retries) {
                    ESP_LOGI(TAG, "Retrying provisioning... (%d/%d)", 
                             ctx.prov_retry_count, ctx.prov_config.max_retries);
                    // Continue provisioning
                } else {
                    // Max retries reached, stop provisioning
                    ESP_LOGE(TAG, "Max provisioning retries reached");
                    
                    // Clear provisioning state
                    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
                    ctx.provisioning_active = false;
                    ctx.prov_retry_count = 0;
                    xSemaphoreGive(ctx.mutex);
                    
                    // Clean up provisioning manager
                    wifi_prov_mgr_stop_provisioning();
                    wifi_prov_mgr_deinit();
                    
                    // Send failure event
                    internal_event_t event = INTERNAL_EVENT_PROVISIONING_FAILED;
                    xQueueSend(ctx.event_queue, &event, 0);
                    success_sent = false;
                }
                break;
            }
                
            case WIFI_PROV_DEINIT:
                ESP_LOGI(TAG, "Provisioning deinitialized");
                break;
                
            default:
                break;
        }
    }
}

static void provisioning_start_softap(void) {
    ESP_LOGI(TAG, "Starting SoftAP provisioning");
    
    // Ensure any existing provisioning is cleaned up
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Ensure we're in AP+STA mode
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // DISABLE POWER SAVE DURING PROVISIONING
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    // Create and configure AP network interface if not exists
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    if (ap_netif) {
        // Stop DHCP server
        esp_netif_dhcps_stop(ap_netif);
        
        // Configure static IP for SoftAP
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(ap_netif, &ip_info);
        
        // Start DHCP server
        esp_netif_dhcps_start(ap_netif);
        
        ESP_LOGI(TAG, "SoftAP IP configured: 192.168.4.1");
    }
    
    // ===== FIXED FOR ESP-IDF v5.5 =====
    // Configuration for provisioning - use NULL for event handler
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    
    // Initialize provisioning manager with error check
    esp_err_t ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init provisioning manager: %s", esp_err_to_name(ret));
        return;
    }
    
    // Set service name and password
    const char *service_name = ctx.prov_config.service_name;
    if (service_name == NULL || strlen(service_name) == 0) {
        service_name = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_SSID;
    }
    
    const char *service_password = ctx.prov_config.service_password;
    
    // Security parameters - USE SECURITY 1 FOR BEST COMPATIBILITY
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = (service_password && strlen(service_password) > 0) ? service_password : NULL;
    
    // Start provisioning service
    ESP_LOGI(TAG, "Starting provisioning with service name: %s", service_name);
    ret = wifi_prov_mgr_start_provisioning(security, pop, service_name, NULL);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        return;
    }
    
    ESP_LOGI(TAG, "Provisioning started with SSID: %s", service_name);
    if (pop) {
        ESP_LOGI(TAG, "Using security with POP");
    } else {
        ESP_LOGI(TAG, "No POP (open network)");
    }
    
    // Debug the SoftAP configuration
    wifi_config_t ap_config;
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_config) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP SSID: %s, Channel: %d", 
                 ap_config.ap.ssid, ap_config.ap.channel);
    }
    
    // Wait for provisioning to complete or timeout
    uint32_t timeout_ms = ctx.prov_config.timeout_ms;
    if (timeout_ms == 0) {
        timeout_ms = CONFIG_WIFI_CLOCK_PROV_SOFT_AP_TIMEOUT_MS;
    }
    
    // ===== REMOVED: wifi_prov_mgr_get_service_name() doesn't exist in your version =====
    // The service name is already set to: service_name
    ESP_LOGI(TAG, "Provisioning service name: %s", service_name);
    // =================================
}

static void provisioning_stop(void) {
    if (!ctx.provisioning_active) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping provisioning");
    
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.provisioning_active = false;
    ctx.status = WIFI_CLOCK_STATUS_DISCONNECTED;
    xSemaphoreGive(ctx.mutex);
    
    if (ctx.user_callback != NULL) {
        ctx.user_callback(WIFI_CLOCK_EVENT_PROVISIONING_STOPPED, NULL);
    }
}

static void provisioning_cleanup(void) {
    // Just clear the flags, don't touch WiFi
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    ctx.provisioning_active = false;
    ctx.prov_retry_count = 0;
    xSemaphoreGive(ctx.mutex);
    
    ESP_LOGI(TAG, "Provisioning cleanup complete");
}

static void reset_wifi_after_provisioning(void) {
    ESP_LOGI(TAG, "Resetting WiFi after provisioning");
    
    // First, stop any ongoing Wi-Fi operations
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Stop Wi-Fi if it's started
    if (ctx.wifi_started) {
        esp_err_t ret = esp_wifi_stop();
        if (ret == ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGI(TAG, "Wi-Fi was not started");
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(ret));
        }
        ctx.wifi_started = false;
    }
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Deinit provisioning manager (ignore errors as it might already be deinited)
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Reset Wi-Fi to a clean state
    ESP_LOGI(TAG, "Reinitializing Wi-Fi for normal operation");
    
    // Set mode to NULL first
    esp_wifi_set_mode(WIFI_MODE_NULL);
    
    // Restart Wi-Fi in STA mode only
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    
    // Restore power save mode
    esp_wifi_set_ps(WIFI_CLOCK_POWER_SAVE_MODE);
    
    // Start Wi-Fi
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(ret));
    } else {
        ctx.wifi_started = true;
        ESP_LOGI(TAG, "Wi-Fi restarted successfully in STA mode");
    }
    
    // Clear any pending reconnect flags
    ctx.pending_reconnect = false;
    ctx.total_retry_count = 0;
    
    ESP_LOGI(TAG, "WiFi reset complete - ready for normal operation");
}

static esp_err_t save_provisioned_credentials(const char *ssid, const char *password) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_str(nvs_handle, WIFI_CLOCK_NVS_SSID_KEY, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_str(nvs_handle, WIFI_CLOCK_NVS_PASSWORD_KEY, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Provisioned credentials saved for SSID: %s", ssid);
    }
    
    return err;
}

static esp_err_t load_provisioned_credentials(char *ssid, size_t ssid_len, 
                                              char *password, size_t pass_len) {
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CLOCK_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, WIFI_CLOCK_NVS_SSID_KEY, NULL, &required_size);
    if (err != ESP_OK || required_size > ssid_len) {
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }
    
    err = nvs_get_str(nvs_handle, WIFI_CLOCK_NVS_SSID_KEY, ssid, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    required_size = 0;
    err = nvs_get_str(nvs_handle, WIFI_CLOCK_NVS_PASSWORD_KEY, NULL, &required_size);
    if (err != ESP_OK || required_size > pass_len) {
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }
    
    err = nvs_get_str(nvs_handle, WIFI_CLOCK_NVS_PASSWORD_KEY, password, &required_size);
    
    nvs_close(nvs_handle);
    return err;
}