#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Backlight modes
typedef enum {
    BACKLIGHT_MODE_MANUAL = 0,   // Manual brightness control
    BACKLIGHT_MODE_AUTO_TIME,    // Auto based on time of day
    BACKLIGHT_MODE_AUTO_SENSOR,  // Auto based on ambient light (future)
    BACKLIGHT_MODE_MAX
} backlight_mode_t;

// Initialize backlight control
void backlight_init(void);

// Set brightness (0-100%)
void set_backlight_brightness(uint8_t brightness_percent);

// Get current brightness
uint8_t get_backlight_brightness(void);

// Fade brightness smoothly
void fade_backlight(uint8_t target_brightness, uint32_t duration_ms);

// Toggle backlight on/off
void toggle_backlight(void);

// Save/load brightness from NVS
void save_brightness_to_nvs(uint8_t brightness);
uint8_t load_brightness_from_nvs(void);

// Time-based auto brightness
void adjust_brightness_for_time_of_day(void);

// Simple UI-less brightness control
void increase_brightness(void);
void decrease_brightness(void);

// Brightness test function
void test_backlight_control(void);

// NEW: Auto/Manual mode control functions
void set_backlight_mode(backlight_mode_t mode);
backlight_mode_t get_backlight_mode(void);
void toggle_auto_mode(void);
void save_mode_to_nvs(backlight_mode_t mode);
backlight_mode_t load_mode_from_nvs(void);
void backlight_update_auto(void);  // Call periodically for auto mode

#ifdef __cplusplus
}
#endif

#endif // BACKLIGHT_H