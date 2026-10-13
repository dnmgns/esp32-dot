#pragma once
// Seeed SenseCAP Watcher W1-A board support for esp32-dot.
// Pins and start-up order follow Seeed's sensecap-watcher.[ch] (reference only).
// Everything uses the ESP-IDF "new" I2C master driver; never mix in the legacy
// driver/i2c.h API (IDF aborts at runtime when both are installed).
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#define PIP_LCD_H_RES 412
#define PIP_LCD_V_RES 412

// General I2C bus 0 (IO expander, audio codecs). Valid after pip_watcher_power_init.
i2c_master_bus_handle_t pip_watcher_i2c(void);

// Powers the board rails through the PCA9535 expander (LCD, SYSTEM, CODEC_PA).
esp_err_t pip_watcher_power_init(void);

// SPD2010 412x412 QSPI panel, awake and displaying, backlight off.
esp_err_t pip_watcher_display_new(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io);
// 0..100 percent, LEDC PWM on GPIO8.
esp_err_t pip_watcher_backlight_set(int percent);

esp_err_t pip_watcher_touch_new(esp_lcd_touch_handle_t *touch);

// Backlight off, LCD/codec rails off, then SYSTEM rail off (Seeed's shutdown; it only
// cuts power on battery). If still running (USB power), deep sleep; a dial press wakes it.
void pip_watcher_power_off(void);

// Rotary dial (GPIO41/42) and push button (expander pin 3) as an LVGL encoder.
// Call while holding the LVGL lock.
lv_indev_t *pip_watcher_knob_add(lv_display_t *display);

// Binds the dial to the UI scene (defined in pip_ui.c; call with the LVGL lock held).
void pip_ui_attach_dial(lv_indev_t *dial);
