#include <stdio.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "backlight.h"

static const char *TAG = "BACKLIGHT";

// Configuration from menuconfig
#define BACKLIGHT_PIN CONFIG_BACKLIGHT_GPIO_NUM

// Backlight control structure
typedef struct {
    uint8_t channel;
    uint8_t duty;           // Current brightness (0-100%)
    bool initialized;
    bool pwm_enabled;
    backlight_mode_t mode;  // Current mode
    uint8_t last_manual_brightness; // Remember last manual brightness
    uint32_t last_auto_update;      // Last auto update time (ms)
} backlight_control_t;

static backlight_control_t backlight = {
    .channel = LEDC_CHANNEL_0,
    .duty = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS,  // Use Kconfig default
    .initialized = false,
    .pwm_enabled = false,
    .mode = BACKLIGHT_MODE_MANUAL,
    .last_manual_brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS,  // Use Kconfig default
    .last_auto_update = 0
};

// Initialize PWM for backlight
static esp_err_t backlight_pwm_init(void) {
    if (backlight.initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing backlight PWM on GPIO %d", BACKLIGHT_PIN);

    // Configure LEDC timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,  // 10-bit resolution (0-1023)
        .freq_hz = 5000,                        // 5kHz frequency
        .clk_cfg = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure LEDC channel
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = backlight.channel,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BACKLIGHT_PIN,
        .duty = 0,          // Start with 0 duty (off)
        .hpoint = 0
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(ret));
        return ret;
    }

    backlight.initialized = true;
    backlight.pwm_enabled = true;
    ESP_LOGI(TAG, "Backlight PWM initialized successfully");
    
    return ESP_OK;
}

// Initialize backlight control
void backlight_init(void) {
    ESP_LOGI(TAG, "Initializing backlight control");
    
    // Set default mode based on Kconfig
    #if defined(CONFIG_BACKLIGHT_DEFAULT_MODE_AUTO)
        backlight.mode = BACKLIGHT_MODE_AUTO_TIME;
        ESP_LOGI(TAG, "Default mode from Kconfig: AUTO");
    #else
        backlight.mode = BACKLIGHT_MODE_MANUAL;
        ESP_LOGI(TAG, "Default mode from Kconfig: MANUAL");
    #endif
    
    // Try to load saved mode and brightness from NVS if enabled
    #ifdef CONFIG_BACKLIGHT_USE_NVS
        backlight_mode_t saved_mode = load_mode_from_nvs();
        uint8_t saved_brightness = load_brightness_from_nvs();
        
        // Ensure values are valid
        if (saved_mode >= BACKLIGHT_MODE_MAX) {
            saved_mode = backlight.mode;  // Use Kconfig default
            ESP_LOGW(TAG, "Invalid mode %d found, resetting to Kconfig default", saved_mode);
            save_mode_to_nvs(saved_mode);
        }
        
        if (saved_brightness > 100) {
            saved_brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
            ESP_LOGW(TAG, "Invalid brightness %d%% found, resetting to Kconfig default %d%%", 
                    saved_brightness, CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS);
            save_brightness_to_nvs(saved_brightness);
        }
        
        backlight.mode = saved_mode;
        backlight.duty = saved_brightness;
        backlight.last_manual_brightness = saved_brightness;
    #endif
    
    ESP_LOGI(TAG, "Setting initial mode: %s, brightness: %d%%", 
             (backlight.mode == BACKLIGHT_MODE_MANUAL) ? "MANUAL" : "AUTO",
             backlight.duty);
    
    // Initialize PWM
    esp_err_t ret = backlight_pwm_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PWM initialization failed, using simple GPIO control");
        
        // Fallback to simple GPIO control
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << BACKLIGHT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        backlight.pwm_enabled = false;
        backlight.initialized = true;
    } else {
        backlight.pwm_enabled = true;
        backlight.initialized = true;
    }
    
    // If in auto mode, calculate initial brightness based on time
    if (backlight.mode == BACKLIGHT_MODE_AUTO_TIME) {
        adjust_brightness_for_time_of_day();
    } else {
        // Manual mode - use saved or default brightness
        set_backlight_brightness(backlight.duty);
    }
    
    backlight.last_auto_update = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    ESP_LOGI(TAG, "Backlight initialized successfully");
}

// Set brightness (0-100%)
void set_backlight_brightness(uint8_t brightness_percent) {
    if (!backlight.initialized) {
        backlight_init();
    }
    
    // Apply min/max constraints from Kconfig
    #ifdef CONFIG_BACKLIGHT_MIN_BRIGHTNESS
        if (brightness_percent < CONFIG_BACKLIGHT_MIN_BRIGHTNESS) {
            brightness_percent = CONFIG_BACKLIGHT_MIN_BRIGHTNESS;
        }
    #endif
    
    #ifdef CONFIG_BACKLIGHT_MAX_BRIGHTNESS
        if (brightness_percent > CONFIG_BACKLIGHT_MAX_BRIGHTNESS) {
            brightness_percent = CONFIG_BACKLIGHT_MAX_BRIGHTNESS;
        }
    #endif
    
    // Clamp brightness to 0-100% (additional safety)
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    
    // Save the brightness percentage
    backlight.duty = brightness_percent;
    
    // Store manual brightness if we're in manual mode
    if (backlight.mode == BACKLIGHT_MODE_MANUAL) {
        backlight.last_manual_brightness = brightness_percent;
    }
    
    if (backlight.pwm_enabled) {
        // Convert percentage to duty cycle (0-1023 for 10-bit)
        uint32_t duty = (brightness_percent * 1023) / 100;
        
        // Set the duty cycle
        esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, backlight.channel, duty);
        if (ret == ESP_OK) {
            ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, backlight.channel);
        }
        
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set PWM duty, falling back to GPIO");
            backlight.pwm_enabled = false;
        } else {
            ESP_LOGI(TAG, "Backlight set to %d%% (PWM duty: %lu)", brightness_percent, duty);
            return;
        }
    }
    
    // Fallback to simple GPIO control
    if (brightness_percent > 50) {
        gpio_set_level(BACKLIGHT_PIN, 1);
        ESP_LOGI(TAG, "Backlight ON (GPIO mode)");
    } else if (brightness_percent > 0) {
        // For simple GPIO, we can only toggle at 50% threshold
        gpio_set_level(BACKLIGHT_PIN, 0);
        ESP_LOGI(TAG, "Backlight OFF (GPIO mode)");
    } else {
        gpio_set_level(BACKLIGHT_PIN, 0);
        ESP_LOGI(TAG, "Backlight OFF");
    }
}

// Get current brightness
uint8_t get_backlight_brightness(void) {
    return backlight.duty;
}

// Set backlight mode
void set_backlight_mode(backlight_mode_t mode) {
    if (mode >= BACKLIGHT_MODE_MAX) {
        ESP_LOGW(TAG, "Invalid mode %d, defaulting to MANUAL", mode);
        mode = BACKLIGHT_MODE_MANUAL;
    }
    
    backlight.mode = mode;
    ESP_LOGI(TAG, "Backlight mode set to: %d", mode);
    
    // Save mode to NVS if enabled
    #ifdef CONFIG_BACKLIGHT_USE_NVS
        save_mode_to_nvs(mode);
    #endif
    
    // If switching to auto mode, update brightness immediately
    if (mode == BACKLIGHT_MODE_AUTO_TIME) {
        adjust_brightness_for_time_of_day();
    } 
    // If switching to manual mode, restore last manual brightness
    else if (mode == BACKLIGHT_MODE_MANUAL) {
        set_backlight_brightness(backlight.last_manual_brightness);
    }
    
    backlight.last_auto_update = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// Get current backlight mode
backlight_mode_t get_backlight_mode(void) {
    return backlight.mode;
}

// Toggle between auto and manual mode
void toggle_auto_mode(void) {
    if (backlight.mode == BACKLIGHT_MODE_MANUAL) {
        set_backlight_mode(BACKLIGHT_MODE_AUTO_TIME);
        ESP_LOGI(TAG, "Switched to AUTO mode");
    } else {
        set_backlight_mode(BACKLIGHT_MODE_MANUAL);
        ESP_LOGI(TAG, "Switched to MANUAL mode");
    }
}

// Increase brightness in steps (only works in manual mode)
void increase_brightness(void) {
    if (backlight.mode != BACKLIGHT_MODE_MANUAL) {
        ESP_LOGI(TAG, "Cannot increase brightness in auto mode. Switch to manual first.");
        return;
    }
    
    uint8_t current = get_backlight_brightness();
    uint8_t new_brightness;
    
    // Increase in steps: 0 -> 20 -> 40 -> 60 -> 80 -> 100
    if (current < 20) new_brightness = 20;
    else if (current < 40) new_brightness = 40;
    else if (current < 60) new_brightness = 60;
    else if (current < 80) new_brightness = 80;
    else new_brightness = 100;
    
    set_backlight_brightness(new_brightness);
    
    #ifdef CONFIG_BACKLIGHT_USE_NVS
        save_brightness_to_nvs(new_brightness);
    #endif
}

// Decrease brightness in steps (only works in manual mode)
void decrease_brightness(void) {
    if (backlight.mode != BACKLIGHT_MODE_MANUAL) {
        ESP_LOGI(TAG, "Cannot decrease brightness in auto mode. Switch to manual first.");
        return;
    }
    
    uint8_t current = get_backlight_brightness();
    uint8_t new_brightness;
    
    // Decrease in steps: 100 -> 80 -> 60 -> 40 -> 20 -> 0
    if (current > 80) new_brightness = 80;
    else if (current > 60) new_brightness = 60;
    else if (current > 40) new_brightness = 40;
    else if (current > 20) new_brightness = 20;
    else new_brightness = 0;
    
    set_backlight_brightness(new_brightness);
    
    #ifdef CONFIG_BACKLIGHT_USE_NVS
        save_brightness_to_nvs(new_brightness);
    #endif
}

// Fade brightness smoothly
void fade_backlight(uint8_t target_brightness, uint32_t duration_ms) {
    if (!backlight.initialized) {
        backlight_init();
    }
    
    uint8_t start_brightness = backlight.duty;
    int16_t steps = target_brightness - start_brightness;
    
    if (steps == 0) return;
    
    // Calculate step delay
    uint32_t step_count = abs(steps);
    uint32_t step_delay = duration_ms / step_count;
    if (step_delay < 10) step_delay = 10;  // Minimum 10ms per step
    
    ESP_LOGI(TAG, "Fading backlight from %d%% to %d%% (%d steps, %lums each)", 
             start_brightness, target_brightness, step_count, step_delay);
    
    // Fade in or out
    for (uint32_t i = 1; i <= step_count; i++) {
        uint8_t current = start_brightness + (steps > 0 ? i : -i);
        set_backlight_brightness(current);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
    
    // Ensure final value is set
    set_backlight_brightness(target_brightness);
}

// Toggle backlight on/off (works in both modes)
void toggle_backlight(void) {
    static bool is_on = true;
    
    if (is_on) {
        ESP_LOGI(TAG, "Turning backlight OFF");
        set_backlight_brightness(0);
    } else {
        ESP_LOGI(TAG, "Turning backlight ON");
        // Restore to appropriate brightness based on mode
        if (backlight.mode == BACKLIGHT_MODE_AUTO_TIME) {
            adjust_brightness_for_time_of_day();
        } else {
            set_backlight_brightness(backlight.last_manual_brightness);
        }
    }
    
    is_on = !is_on;
}

// Save mode to NVS
#ifdef CONFIG_BACKLIGHT_USE_NVS
void save_mode_to_nvs(backlight_mode_t mode) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, "backlight_mode", (uint8_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Mode %d saved to NVS", mode);
            } else {
                ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Failed to save mode to NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    }
}

// Load mode from NVS
backlight_mode_t load_mode_from_nvs(void) {
    nvs_handle_t nvs_handle;
    uint8_t mode = backlight.mode;  // Default to Kconfig default
    
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, "backlight_mode", &mode);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded mode from NVS: %d", mode);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "No mode saved in NVS, using Kconfig default");
            mode = backlight.mode;
            // Save default to NVS
            nvs_close(nvs_handle);
            save_mode_to_nvs(mode);
            return mode;
        } else {
            ESP_LOGE(TAG, "Error reading mode from NVS: %s", esp_err_to_name(err));
            mode = backlight.mode;
        }
        nvs_close(nvs_handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace 'storage' not found, creating with Kconfig default");
        mode = backlight.mode;
        // Save default to NVS
        save_mode_to_nvs(mode);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s, using Kconfig default", esp_err_to_name(err));
        mode = backlight.mode;
    }
    
    // Ensure mode is valid
    if (mode >= BACKLIGHT_MODE_MAX) {
        mode = backlight.mode;
    }
    
    return (backlight_mode_t)mode;
}

// Save brightness to NVS
void save_brightness_to_nvs(uint8_t brightness) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, "brightness", brightness);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Brightness %d%% saved to NVS", brightness);
            } else {
                ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "Failed to save brightness to NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS: %s - Creating new entry", esp_err_to_name(err));
        
        // Try to create a new NVS entry
        err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(nvs_handle, "brightness", brightness);
            if (err == ESP_OK) {
                nvs_commit(nvs_handle);
                ESP_LOGI(TAG, "Created new NVS entry for brightness: %d%%", brightness);
            }
            nvs_close(nvs_handle);
        }
    }
}

// Load brightness from NVS
uint8_t load_brightness_from_nvs(void) {
    nvs_handle_t nvs_handle;
    uint8_t brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
    
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, "brightness", &brightness);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded brightness from NVS: %d%%", brightness);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "No brightness saved in NVS, using Kconfig default %d%%", 
                    CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS);
            brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
            // Save default to NVS
            nvs_close(nvs_handle);
            save_brightness_to_nvs(brightness);
            return brightness;
        } else {
            ESP_LOGE(TAG, "Error reading brightness from NVS: %s", esp_err_to_name(err));
            brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
        }
        nvs_close(nvs_handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace 'storage' not found, creating with Kconfig default %d%%", 
                CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS);
        brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
        // Save default to NVS
        save_brightness_to_nvs(brightness);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s, using Kconfig default %d%%", 
                esp_err_to_name(err), CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS);
        brightness = CONFIG_BACKLIGHT_DEFAULT_BRIGHTNESS;
    }
    
    return brightness;
}
#endif /* CONFIG_BACKLIGHT_USE_NVS */

// Adjust brightness based on time of day
void adjust_brightness_for_time_of_day(void) {
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);

    // Check if time is properly set
    if (timeinfo.tm_year + 1900 < 2020) {
        // Time is not set (year is before 2020)
        ESP_LOGW(TAG, "System time not set! Cannot adjust auto brightness.");
        
        // Fall back to saved brightness
        uint8_t fallback_brightness = backlight.last_manual_brightness;
        
        // Only set if different from current
        if (fallback_brightness != backlight.duty) {
            ESP_LOGI(TAG, "Using saved brightness %d%% until time is set", fallback_brightness);
            set_backlight_brightness(fallback_brightness);
        }
        return;
    }
    
    int hour = timeinfo.tm_hour;
    uint8_t target_brightness;
    
    // Adjust brightness based on time using Kconfig values
    if (hour >= 22 || hour < 6) {
        target_brightness = CONFIG_BACKLIGHT_AUTO_NIGHT_BRIGHTNESS;
    } else if (hour >= 6 && hour < 8) {
        target_brightness = CONFIG_BACKLIGHT_AUTO_MORNING_BRIGHTNESS;
    } else if (hour >= 8 && hour < 18) {
        target_brightness = CONFIG_BACKLIGHT_AUTO_DAY_BRIGHTNESS;
    } else if (hour >= 18 && hour < 20) {
        target_brightness = CONFIG_BACKLIGHT_AUTO_EVENING_BRIGHTNESS;
    } else {
        target_brightness = CONFIG_BACKLIGHT_AUTO_LATE_EVENING_BRIGHTNESS;
    }
    
    // Only change if significantly different (more than 20%)
    uint8_t current = get_backlight_brightness();
    if (target_brightness != current) {
        ESP_LOGI(TAG, "Auto-brightness: %d%% -> %d%% (hour: %d)", 
                current, target_brightness, hour);
        set_backlight_brightness(target_brightness);
        // Don't save to NVS in auto mode - we want to keep manual brightness
    }
}

// Update auto brightness (call this periodically, e.g., every minute)
void backlight_update_auto(void) {
    if (!backlight.initialized) {
        return;
    }
    
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Update auto brightness every 5 minutes (300000 ms)
    if (current_time - backlight.last_auto_update > 300000) {
        if (backlight.mode == BACKLIGHT_MODE_AUTO_TIME) {
            adjust_brightness_for_time_of_day();
        }
        backlight.last_auto_update = current_time;
    }
}

// Test function to verify backlight works
void test_backlight_control(void) {
    ESP_LOGI(TAG, "=== STARTING BACKLIGHT TEST ===");
    
    // Test mode switching
    ESP_LOGI(TAG, "Test 1: Switching to AUTO mode");
    set_backlight_mode(BACKLIGHT_MODE_AUTO_TIME);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Test 2: Switching to MANUAL mode");
    set_backlight_mode(BACKLIGHT_MODE_MANUAL);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 3: Toggle auto mode
    ESP_LOGI(TAG, "Test 3: Toggling auto mode");
    toggle_auto_mode();
    vTaskDelay(pdMS_TO_TICKS(2000));
    toggle_auto_mode();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 4: Try to change brightness in auto mode (should be blocked)
    ESP_LOGI(TAG, "Test 4: Trying to change brightness in auto mode");
    set_backlight_mode(BACKLIGHT_MODE_AUTO_TIME);
    increase_brightness();  // Should log a warning
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 5: Change brightness in manual mode
    ESP_LOGI(TAG, "Test 5: Changing brightness in manual mode");
    set_backlight_mode(BACKLIGHT_MODE_MANUAL);
    increase_brightness();
    vTaskDelay(pdMS_TO_TICKS(1000));
    decrease_brightness();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Continue with other tests...
    
    ESP_LOGI(TAG, "=== BACKLIGHT TEST COMPLETE ===");
}