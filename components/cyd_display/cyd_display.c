#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_xpt2046.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cyd_display.h"

static const char *TAG = "CYD_DISPLAY";

// ========== Configuration from Kconfig ==========
// Display pins
#define DISPLAY_WIDTH           CONFIG_CYD_DISPLAY_WIDTH
#define DISPLAY_HEIGHT          CONFIG_CYD_DISPLAY_HEIGHT
#define PIN_NUM_MOSI            CONFIG_CYD_DISPLAY_SPI_MOSI
#define PIN_NUM_MISO            CONFIG_CYD_DISPLAY_SPI_MISO
#define PIN_NUM_CLK             CONFIG_CYD_DISPLAY_SPI_SCLK
#define PIN_NUM_CS              CONFIG_CYD_DISPLAY_SPI_CS
#define PIN_NUM_DC              CONFIG_CYD_DISPLAY_SPI_DC
#define PIN_NUM_RST             CONFIG_CYD_DISPLAY_SPI_RST
#define DISPLAY_SPI_FREQ        (CONFIG_CYD_DISPLAY_SPI_FREQ * 1000 * 1000)
#define LVGL_BUF_LINES          CONFIG_CYD_DISPLAY_BUFFER_LINES

// Touch pins
#ifdef CONFIG_CYD_TOUCH_ENABLED
    #define PIN_NUM_TOUCH_CS    CONFIG_CYD_TOUCH_SPI_CS
    #define PIN_NUM_TOUCH_IRQ   CONFIG_CYD_TOUCH_SPI_IRQ
    #define TOUCH_SPI_FREQ      (CONFIG_CYD_TOUCH_SPI_FREQ * 1000 * 1000)
#endif

// Backlight pins
#ifdef CONFIG_CYD_BACKLIGHT_ENABLED
    #define BACKLIGHT_PIN       CONFIG_CYD_BACKLIGHT_GPIO_NUM
    #define DEFAULT_BRIGHTNESS  CONFIG_CYD_BACKLIGHT_DEFAULT_BRIGHTNESS
    #define MIN_BRIGHTNESS      CONFIG_CYD_BACKLIGHT_MIN_BRIGHTNESS
    #define MAX_BRIGHTNESS      CONFIG_CYD_BACKLIGHT_MAX_BRIGHTNESS
#endif

// SPI bus
#define LCD_HOST    SPI2_HOST

// ========== Global Handles ==========
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;
static lv_disp_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_indev = NULL;

// LVGL display buffer
static lv_color_t *lvgl_buf = NULL;
static lv_disp_draw_buf_t disp_buf;

// ========== Backlight Control Structure ==========
#ifdef CONFIG_CYD_BACKLIGHT_ENABLED
typedef struct {
    uint8_t channel;
    uint8_t duty;                    // Current brightness (0-100%)
    bool initialized;
    bool pwm_enabled;
    cyd_backlight_mode_t mode;       // Current mode
    uint8_t last_manual_brightness;  // Remember last manual brightness
    uint32_t last_auto_update;       // Last auto update time (ms)
} backlight_control_t;

static backlight_control_t backlight = {
    .channel = LEDC_CHANNEL_0,
    .duty = DEFAULT_BRIGHTNESS,
    .initialized = false,
    .pwm_enabled = false,
    .mode = CYD_BACKLIGHT_MODE_MANUAL,
    .last_manual_brightness = DEFAULT_BRIGHTNESS,
    .last_auto_update = 0
};
#endif // CONFIG_CYD_BACKLIGHT_ENABLED

// ========== Forward Declarations ==========
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

#ifdef CONFIG_CYD_BACKLIGHT_ENABLED
static esp_err_t backlight_pwm_init(void);
static void backlight_gpio_fallback_init(void);
#endif

// ========== Display Initialization ==========
static esp_err_t spi_bus_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus");
    
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * LVGL_BUF_LINES * sizeof(uint16_t),
    };
    
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "SPI bus initialized successfully");
    return ESP_OK;
}

static esp_err_t display_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7796 display panel");
    
    // Initialize panel IO
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = DISPLAY_SPI_FREQ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    
    ret = esp_lcd_new_panel_st7796(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7796 panel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Reset and initialize panel
    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset panel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize panel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure display orientation
    ret = esp_lcd_panel_swap_xy(panel_handle, CONFIG_CYD_DISPLAY_SWAP_XY);
    ret = esp_lcd_panel_mirror(panel_handle, CONFIG_CYD_DISPLAY_MIRROR_X, CONFIG_CYD_DISPLAY_MIRROR_Y);
    ret = esp_lcd_panel_invert_color(panel_handle, CONFIG_CYD_DISPLAY_INVERT_COLOR);
    
    // Turn on display
    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn on display: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Display panel initialized successfully");
    return ESP_OK;
}

#ifdef CONFIG_CYD_TOUCH_ENABLED
static esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "Initializing XPT2046 touch controller");
    
    // Configure touch panel IO (SPI)
    esp_lcd_panel_io_spi_config_t tp_io_config = {
        .dc_gpio_num = GPIO_NUM_NC,
        .cs_gpio_num = PIN_NUM_TOUCH_CS,
        .pclk_hz = TOUCH_SPI_FREQ,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch panel IO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure touch
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH,
        .y_max = DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = PIN_NUM_TOUCH_IRQ,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = CONFIG_CYD_TOUCH_SWAP_XY,
            .mirror_x = CONFIG_CYD_TOUCH_MIRROR_X,
            .mirror_y = CONFIG_CYD_TOUCH_MIRROR_Y,
        },
    };
    
    ret = esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create XPT2046 touch: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Touch controller initialized successfully");
    return ESP_OK;
}
#endif // CONFIG_CYD_TOUCH_ENABLED

static esp_err_t lvgl_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL");
    
    // Initialize LVGL
    lv_init();
    
    // Allocate LVGL display buffer
    size_t buffer_size = DISPLAY_WIDTH * LVGL_BUF_LINES * sizeof(lv_color_t);
    lvgl_buf = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (lvgl_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer (%u bytes)", buffer_size);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize LVGL draw buffer
    lv_disp_draw_buf_init(&disp_buf, lvgl_buf, NULL, DISPLAY_WIDTH * LVGL_BUF_LINES);
    
    // Initialize display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    
    // Register display driver
    lvgl_disp = lv_disp_drv_register(&disp_drv);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to register LVGL display driver");
        return ESP_FAIL;
    }
    
    // Initialize input device driver if touch is enabled
    #ifdef CONFIG_CYD_TOUCH_ENABLED
    if (touch_handle != NULL) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = lvgl_touch_read_cb;
        indev_drv.user_data = touch_handle;
        
        lvgl_indev = lv_indev_drv_register(&indev_drv);
        if (lvgl_indev == NULL) {
            ESP_LOGW(TAG, "Failed to register LVGL input driver");
        } else {
            ESP_LOGI(TAG, "Touch input device registered");
        }
    }
    #endif
    
    ESP_LOGI(TAG, "LVGL initialized successfully");
    return ESP_OK;
}

// ========== LVGL Callbacks ==========
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t) drv->user_data;
    
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    esp_lcd_panel_draw_bitmap(panel, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

#ifdef CONFIG_CYD_TOUCH_ENABLED
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t) drv->user_data;
    
    uint16_t touch_x[1] = {0};
    uint16_t touch_y[1] = {0};
    uint8_t touch_cnt = 0;
    
    esp_lcd_touch_read_data(touch);
    bool touched = esp_lcd_touch_get_coordinates(touch, touch_x, touch_y, NULL, &touch_cnt, 1);
    
    if (touched && touch_cnt > 0) {
        data->point.x = touch_x[0];
        data->point.y = touch_y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif // CONFIG_CYD_TOUCH_ENABLED

// ========== Backlight Functions ==========
#ifdef CONFIG_CYD_BACKLIGHT_ENABLED

static esp_err_t backlight_pwm_init(void)
{
    if (backlight.initialized && backlight.pwm_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing backlight PWM on GPIO %d", BACKLIGHT_PIN);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = backlight.channel,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BACKLIGHT_PIN,
        .duty = 0,
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

static void backlight_gpio_fallback_init(void)
{
    ESP_LOGW(TAG, "Using GPIO fallback for backlight control");
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
}

void cyd_display_set_backlight_brightness(uint8_t brightness_percent)
{
    if (!backlight.initialized) {
        return;
    }
    
    // Apply min/max constraints
    #ifdef MIN_BRIGHTNESS
        if (brightness_percent < MIN_BRIGHTNESS) {
            brightness_percent = MIN_BRIGHTNESS;
        }
    #endif
    
    #ifdef MAX_BRIGHTNESS
        if (brightness_percent > MAX_BRIGHTNESS) {
            brightness_percent = MAX_BRIGHTNESS;
        }
    #endif
    
    if (brightness_percent > 100) brightness_percent = 100;
    
    backlight.duty = brightness_percent;
    
    if (backlight.mode == CYD_BACKLIGHT_MODE_MANUAL) {
        backlight.last_manual_brightness = brightness_percent;
    }
    
    if (backlight.pwm_enabled) {
        uint32_t duty = (brightness_percent * 1023) / 100;
        
        esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, backlight.channel, duty);
        if (ret == ESP_OK) {
            ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, backlight.channel);
        }
        
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Backlight set to %d%% (PWM)", brightness_percent);
            return;
        } else {
            ESP_LOGW(TAG, "PWM failed, falling back to GPIO");
            backlight.pwm_enabled = false;
        }
    }
    
    // GPIO fallback
    gpio_set_level(BACKLIGHT_PIN, brightness_percent > 50 ? 1 : 0);
}

uint8_t cyd_display_get_backlight_brightness(void)
{
    return backlight.duty;
}

void cyd_display_fade_backlight(uint8_t target_brightness, uint32_t duration_ms)
{
    uint8_t start_brightness = backlight.duty;
    int16_t steps = target_brightness - start_brightness;
    
    if (steps == 0) return;
    
    uint32_t step_count = abs(steps);
    uint32_t step_delay = duration_ms / step_count;
    if (step_delay < 10) step_delay = 10;
    
    for (uint32_t i = 1; i <= step_count; i++) {
        uint8_t current = start_brightness + (steps > 0 ? i : -i);
        cyd_display_set_backlight_brightness(current);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
    
    cyd_display_set_backlight_brightness(target_brightness);
}

void cyd_display_toggle_backlight(void)
{
    static bool is_on = true;
    
    if (is_on) {
        cyd_display_set_backlight_brightness(0);
    } else {
        if (backlight.mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
            cyd_display_adjust_brightness_for_time();
        } else {
            cyd_display_set_backlight_brightness(backlight.last_manual_brightness);
        }
    }
    
    is_on = !is_on;
}

void cyd_display_set_backlight_mode(cyd_backlight_mode_t mode)
{
    if (mode >= CYD_BACKLIGHT_MODE_MAX) {
        mode = CYD_BACKLIGHT_MODE_MANUAL;
    }
    
    backlight.mode = mode;
    ESP_LOGI(TAG, "Backlight mode set to: %s", 
             mode == CYD_BACKLIGHT_MODE_MANUAL ? "MANUAL" : "AUTO");
    
    #ifdef CONFIG_CYD_BACKLIGHT_USE_NVS
        cyd_display_save_mode_to_nvs(mode);
    #endif
    
    if (mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        cyd_display_adjust_brightness_for_time();
    } else {
        cyd_display_set_backlight_brightness(backlight.last_manual_brightness);
    }
    
    backlight.last_auto_update = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

cyd_backlight_mode_t cyd_display_get_backlight_mode(void)
{
    return backlight.mode;
}

void cyd_display_toggle_auto_mode(void)
{
    if (backlight.mode == CYD_BACKLIGHT_MODE_MANUAL) {
        cyd_display_set_backlight_mode(CYD_BACKLIGHT_MODE_AUTO_TIME);
    } else {
        cyd_display_set_backlight_mode(CYD_BACKLIGHT_MODE_MANUAL);
    }
}

void cyd_display_increase_brightness(void)
{
    if (backlight.mode != CYD_BACKLIGHT_MODE_MANUAL) {
        ESP_LOGW(TAG, "Cannot adjust brightness in auto mode");
        return;
    }
    
    uint8_t current = cyd_display_get_backlight_brightness();
    uint8_t new_brightness;
    
    if (current < 20) new_brightness = 20;
    else if (current < 40) new_brightness = 40;
    else if (current < 60) new_brightness = 60;
    else if (current < 80) new_brightness = 80;
    else new_brightness = 100;
    
    cyd_display_set_backlight_brightness(new_brightness);
    
    #ifdef CONFIG_CYD_BACKLIGHT_USE_NVS
        cyd_display_save_brightness_to_nvs(new_brightness);
    #endif
}

void cyd_display_decrease_brightness(void)
{
    if (backlight.mode != CYD_BACKLIGHT_MODE_MANUAL) {
        ESP_LOGW(TAG, "Cannot adjust brightness in auto mode");
        return;
    }
    
    uint8_t current = cyd_display_get_backlight_brightness();
    uint8_t new_brightness;
    
    if (current > 80) new_brightness = 80;
    else if (current > 60) new_brightness = 60;
    else if (current > 40) new_brightness = 40;
    else if (current > 20) new_brightness = 20;
    else new_brightness = 0;
    
    cyd_display_set_backlight_brightness(new_brightness);
    
    #ifdef CONFIG_CYD_BACKLIGHT_USE_NVS
        cyd_display_save_brightness_to_nvs(new_brightness);
    #endif
}

void cyd_display_adjust_brightness_for_time(void)
{
    time_t now;
    struct tm timeinfo;
    
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year + 1900 < 2020) {
        ESP_LOGW(TAG, "System time not set! Using saved brightness");
        cyd_display_set_backlight_brightness(backlight.last_manual_brightness);
        return;
    }
    
    int hour = timeinfo.tm_hour;
    uint8_t target_brightness;
    
    if (hour >= 22 || hour < 6) {
        target_brightness = CONFIG_CYD_BACKLIGHT_AUTO_NIGHT_BRIGHTNESS;
    } else if (hour >= 6 && hour < 8) {
        target_brightness = CONFIG_CYD_BACKLIGHT_AUTO_MORNING_BRIGHTNESS;
    } else if (hour >= 8 && hour < 18) {
        target_brightness = CONFIG_CYD_BACKLIGHT_AUTO_DAY_BRIGHTNESS;
    } else if (hour >= 18 && hour < 20) {
        target_brightness = CONFIG_CYD_BACKLIGHT_AUTO_EVENING_BRIGHTNESS;
    } else {
        target_brightness = CONFIG_CYD_BACKLIGHT_AUTO_LATE_EVENING_BRIGHTNESS;
    }
    
    if (target_brightness != backlight.duty) {
        ESP_LOGI(TAG, "Auto-brightness: %d%% (hour: %d)", target_brightness, hour);
        cyd_display_set_backlight_brightness(target_brightness);
    }
}

void cyd_display_update_auto(void)
{
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (current_time - backlight.last_auto_update > 300000) {
        if (backlight.mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
            cyd_display_adjust_brightness_for_time();
        }
        backlight.last_auto_update = current_time;
    }
}

#ifdef CONFIG_CYD_BACKLIGHT_USE_NVS
void cyd_display_save_brightness_to_nvs(uint8_t brightness)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("cyd_display", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, "brightness", brightness);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Brightness %d%% saved to NVS", brightness);
        }
        nvs_close(nvs_handle);
    }
}

uint8_t cyd_display_load_brightness_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    uint8_t brightness = DEFAULT_BRIGHTNESS;
    
    esp_err_t err = nvs_open("cyd_display", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, "brightness", &brightness);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded brightness from NVS: %d%%", brightness);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Save default
            nvs_close(nvs_handle);
            cyd_display_save_brightness_to_nvs(brightness);
        }
        nvs_close(nvs_handle);
    }
    
    return brightness;
}

void cyd_display_save_mode_to_nvs(cyd_backlight_mode_t mode)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("cyd_display", NVS_READWRITE, &nvs_handle);
    
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, "bl_mode", (uint8_t)mode);
        if (err == ESP_OK) {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "Mode %d saved to NVS", mode);
        }
        nvs_close(nvs_handle);
    }
}

cyd_backlight_mode_t cyd_display_load_mode_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    uint8_t mode = CYD_BACKLIGHT_MODE_MANUAL;
    
    #ifdef CONFIG_CYD_BACKLIGHT_DEFAULT_MODE_AUTO
        mode = CYD_BACKLIGHT_MODE_AUTO_TIME;
    #endif
    
    esp_err_t err = nvs_open("cyd_display", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        uint8_t saved_mode;
        err = nvs_get_u8(nvs_handle, "bl_mode", &saved_mode);
        if (err == ESP_OK && saved_mode < CYD_BACKLIGHT_MODE_MAX) {
            mode = saved_mode;
            ESP_LOGI(TAG, "Loaded mode from NVS: %d", mode);
        }
        nvs_close(nvs_handle);
    }
    
    return (cyd_backlight_mode_t)mode;
}
#endif // CONFIG_CYD_BACKLIGHT_USE_NVS

void cyd_display_test_backlight(void)
{
    ESP_LOGI(TAG, "=== BACKLIGHT TEST ===");
    
    cyd_backlight_mode_t original_mode = backlight.mode;
    
    cyd_display_set_backlight_mode(CYD_BACKLIGHT_MODE_MANUAL);
    
    // Test brightness levels
    uint8_t levels[] = {0, 20, 40, 60, 80, 100, 80, 60, 40, 20, 0};
    for (int i = 0; i < sizeof(levels); i++) {
        cyd_display_set_backlight_brightness(levels[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Test fade
    cyd_display_fade_backlight(80, 2000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    cyd_display_fade_backlight(20, 2000);
    
    // Restore original mode
    cyd_display_set_backlight_mode(original_mode);
    
    ESP_LOGI(TAG, "=== BACKLIGHT TEST COMPLETE ===");
}
#endif // CONFIG_CYD_BACKLIGHT_ENABLED

// ========== Public API Implementation ==========
esp_err_t cyd_display_init(void)
{
    ESP_LOGI(TAG, "Initializing CYD display subsystem");
    ESP_LOGI(TAG, "Display: %dx%d ST7796", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    // Initialize SPI bus
    ESP_ERROR_CHECK(spi_bus_init());
    
    // Initialize display panel
    ESP_ERROR_CHECK(display_panel_init());
    
    #ifdef CONFIG_CYD_TOUCH_ENABLED
    // Initialize touch controller
    ESP_ERROR_CHECK(touch_init());
    #endif
    
    #ifdef CONFIG_CYD_BACKLIGHT_ENABLED
    // Initialize backlight
    ESP_LOGI(TAG, "Initializing backlight on GPIO %d", BACKLIGHT_PIN);
    
    #ifdef CONFIG_CYD_BACKLIGHT_USE_NVS
        backlight.mode = cyd_display_load_mode_from_nvs();
        backlight.duty = cyd_display_load_brightness_from_nvs();
        backlight.last_manual_brightness = backlight.duty;
    #else
        #ifdef CONFIG_CYD_BACKLIGHT_DEFAULT_MODE_AUTO
            backlight.mode = CYD_BACKLIGHT_MODE_AUTO_TIME;
        #endif
    #endif
    
    esp_err_t ret = backlight_pwm_init();
    if (ret != ESP_OK) {
        backlight_gpio_fallback_init();
    }
    
    if (backlight.mode == CYD_BACKLIGHT_MODE_AUTO_TIME) {
        cyd_display_adjust_brightness_for_time();
    } else {
        cyd_display_set_backlight_brightness(backlight.duty);
    }
    #endif // CONFIG_CYD_BACKLIGHT_ENABLED
    
    // Initialize LVGL with our drivers
    ESP_ERROR_CHECK(lvgl_init());
    
    ESP_LOGI(TAG, "CYD display subsystem initialized successfully");
    return ESP_OK;
}

lv_disp_t* cyd_display_get_lvgl_disp(void)
{
    return lvgl_disp;
}

lv_indev_t* cyd_display_get_lvgl_indev(void)
{
    return lvgl_indev;
}

bool cyd_display_get_touch_point(uint16_t *x, uint16_t *y)
{
    #ifdef CONFIG_CYD_TOUCH_ENABLED
    if (touch_handle == NULL || x == NULL || y == NULL) {
        return false;
    }
    
    uint16_t touch_x[1] = {0};
    uint16_t touch_y[1] = {0};
    uint8_t touch_cnt = 0;
    
    esp_lcd_touch_read_data(touch_handle);
    return esp_lcd_touch_get_coordinates(touch_handle, touch_x, touch_y, NULL, &touch_cnt, 1);
    #else
    return false;
    #endif
}

uint16_t cyd_display_get_width(void)
{
    return DISPLAY_WIDTH;
}

uint16_t cyd_display_get_height(void)
{
    return DISPLAY_HEIGHT;
}