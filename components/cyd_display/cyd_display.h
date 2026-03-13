#ifndef CYD_DISPLAY_H
#define CYD_DISPLAY_H

#include "esp_err.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== Display Modes ==========
typedef enum {
    CYD_BACKLIGHT_MODE_MANUAL = 0,   // Manual brightness control
    CYD_BACKLIGHT_MODE_AUTO_TIME,    // Auto based on time of day
    CYD_BACKLIGHT_MODE_MAX
} cyd_backlight_mode_t;

// ========== Initialization ==========
/**
 * @brief Initialize CYD display subsystem (display, touch, backlight)
 * 
 * @return esp_err_t ESP_OK on success, error otherwise
 */
esp_err_t cyd_display_init(void);

// ========== Display Functions ==========
/**
 * @brief Get the LVGL display driver handle
 * 
 * @return lv_display_t* Pointer to LVGL display driver (LVGL 9.3 uses lv_display_t)
 */
lv_display_t* cyd_display_get_lvgl_disp(void);  // Changed from lv_disp_t to lv_display_t

/**
 * @brief Get the LVGL input device driver handle
 * 
 * @return lv_indev_t* Pointer to LVGL input device driver
 */
lv_indev_t* cyd_display_get_lvgl_indev(void);

/**
 * @brief Get the display width in pixels
 * 
 * @return uint16_t Display width
 */
uint16_t cyd_display_get_width(void);

/**
 * @brief Get the display height in pixels
 * 
 * @return uint16_t Display height
 */
uint16_t cyd_display_get_height(void);

// ========== Touch Functions ==========
/**
 * @brief Check if touch is currently pressed and get coordinates
 * 
 * @param x Pointer to store X coordinate
 * @param y Pointer to store Y coordinate
 * @return true if touch is pressed, false otherwise
 */
bool cyd_display_get_touch_point(uint16_t *x, uint16_t *y);

// ========== Backlight Functions ==========
/**
 * @brief Set backlight brightness (0-100%)
 * 
 * @param brightness_percent Brightness percentage
 */
void cyd_display_set_backlight_brightness(uint8_t brightness_percent);

/**
 * @brief Get current backlight brightness
 * 
 * @return uint8_t Current brightness percentage
 */
uint8_t cyd_display_get_backlight_brightness(void);

/**
 * @brief Fade backlight smoothly
 * 
 * @param target_brightness Target brightness percentage
 * @param duration_ms Fade duration in milliseconds
 */
void cyd_display_fade_backlight(uint8_t target_brightness, uint32_t duration_ms);

/**
 * @brief Toggle backlight on/off
 */
void cyd_display_toggle_backlight(void);

/**
 * @brief Set backlight mode
 * 
 * @param mode Backlight mode (manual/auto)
 */
void cyd_display_set_backlight_mode(cyd_backlight_mode_t mode);

/**
 * @brief Get current backlight mode
 * 
 * @return cyd_backlight_mode_t Current mode
 */
cyd_backlight_mode_t cyd_display_get_backlight_mode(void);

/**
 * @brief Toggle between auto and manual mode
 */
void cyd_display_toggle_auto_mode(void);

/**
 * @brief Increase brightness in steps (only works in manual mode)
 */
void cyd_display_increase_brightness(void);

/**
 * @brief Decrease brightness in steps (only works in manual mode)
 */
void cyd_display_decrease_brightness(void);

/**
 * @brief Adjust brightness based on time of day (for auto mode)
 */
void cyd_display_adjust_brightness_for_time(void);

/**
 * @brief Update auto brightness (call periodically)
 */
void cyd_display_update_auto(void);

// ========== NVS Functions ==========
/**
 * @brief Save brightness to NVS
 * 
 * @param brightness Brightness percentage to save
 */
void cyd_display_save_brightness_to_nvs(uint8_t brightness);

/**
 * @brief Load brightness from NVS
 * 
 * @return uint8_t Loaded brightness percentage
 */
uint8_t cyd_display_load_brightness_from_nvs(void);

/**
 * @brief Save mode to NVS
 * 
 * @param mode Backlight mode to save
 */
void cyd_display_save_mode_to_nvs(cyd_backlight_mode_t mode);

/**
 * @brief Load mode from NVS
 * 
 * @return cyd_backlight_mode_t Loaded mode
 */
cyd_backlight_mode_t cyd_display_load_mode_from_nvs(void);

// ========== Test Functions ==========
/**
 * @brief Run backlight test sequence
 */
void cyd_display_test_backlight(void);

#ifdef __cplusplus
}
#endif

#endif // CYD_DISPLAY_H