#include "pip_watcher.h"

#include <stdatomic.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_knob.h"

static const char *TAG = "pip_watcher";

// QSPI display bus (SPI3_HOST)
enum { LCD_CLK = 7, LCD_D0 = 9, LCD_D1 = 1, LCD_D2 = 14, LCD_D3 = 13, LCD_CS = 45, LCD_BL = 8 };
// Touch controller bus (I2C1)
enum { TOUCH_SDA = 39, TOUCH_SCL = 38 };
// General bus (I2C0): PCA9535, ES7243, ES8311
enum { GENERAL_SDA = 47, GENERAL_SCL = 48 };
enum { KNOB_A = 41, KNOB_B = 42 };

// PCA9535 at 0x21; pin n of the expander is bit n (P0.0-7 = 0-7, P1.0-7 = 8-15).
enum { EXPANDER_ADDRESS = 0x21 };
enum { EXP_REG_INPUT = 0x00, EXP_REG_OUTPUT = 0x02, EXP_REG_CONFIG = 0x06 };
enum {
    PIN_KNOB_BTN = 1 << 3,
    PIN_PWR_LCD = 1 << 9,
    PIN_PWR_SYSTEM = 1 << 10,
    PIN_PWR_CODEC_PA = 1 << 12,
    // Seeed's DRV_IO_EXP_INPUT_MASK: pins 0-7 and pin 13 (BAT_DET) are inputs,
    // everything else drives a rail. Its source comment says "P1.3", but the
    // numeric value is what the BSP programs. Config bit 1 = input.
    EXP_INPUT_MASK = 0x20ff,
};

static i2c_master_bus_handle_t general_bus;
static i2c_master_dev_handle_t expander;
static uint16_t expander_output;

i2c_master_bus_handle_t pip_watcher_i2c(void) { return general_bus; }

static esp_err_t expander_write(uint8_t reg, uint16_t value)
{
    const uint8_t data[] = {reg, value & 0xff, value >> 8};
    return i2c_master_transmit(expander, data, sizeof(data), 100);
}

static esp_err_t expander_read_inputs(uint16_t *value)
{
    const uint8_t reg = EXP_REG_INPUT;
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(expander, &reg, 1, data, sizeof(data), 100), TAG, "expander read");
    *value = data[0] | (data[1] << 8);
    return ESP_OK;
}

static esp_err_t expander_set_outputs(uint16_t mask, bool on)
{
    expander_output = on ? (expander_output | mask) : (expander_output & ~mask);
    return expander_write(EXP_REG_OUTPUT, expander_output);
}

esp_err_t pip_watcher_power_init(void)
{
    if (general_bus) return ESP_OK;
    // Seeed holds the display and touch lines low before the rails come up, so
    // the SPD2010 never sees floating inputs.
    const uint32_t quiet_pins[] = {TOUCH_SDA, TOUCH_SCL, LCD_CLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CS, LCD_BL};
    for (unsigned i = 0; i < sizeof(quiet_pins) / sizeof(quiet_pins[0]); i++) {
        gpio_reset_pin(quiet_pins[i]);
        gpio_set_direction(quiet_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(quiet_pins[i], 0);
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GENERAL_SDA,
        .scl_io_num = GENERAL_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   // external pull-ups on the board
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &general_bus), TAG, "general I2C bus");
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXPANDER_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(general_bus, &device_config, &expander), TAG, "expander device");

    // Outputs low first, then SYSTEM, then the remaining rails (Seeed: 100 ms, 50 ms).
    // The AI chip (Himax), microSD and Grove rails stay off; this app does not use them.
    expander_output = 0;
    ESP_RETURN_ON_ERROR(expander_write(EXP_REG_OUTPUT, 0), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(expander_write(EXP_REG_CONFIG, EXP_INPUT_MASK), TAG, "expander direction");
    ESP_RETURN_ON_ERROR(expander_set_outputs(PIN_PWR_SYSTEM, true), TAG, "system power");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(expander_set_outputs(PIN_PWR_LCD | PIN_PWR_CODEC_PA, true), TAG, "peripheral power");
    vTaskDelay(pdMS_TO_TICKS(50));
    uint16_t inputs = 0;
    ESP_RETURN_ON_ERROR(expander_read_inputs(&inputs), TAG, "expander inputs");
    ESP_LOGI(TAG, "IO expander ready; inputs=%04x outputs=%04x", inputs, expander_output);
    return ESP_OK;
}

esp_err_t pip_watcher_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint32_t duty = (BIT(LEDC_TIMER_10_BIT) * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void pip_watcher_power_off(void)
{
    pip_watcher_backlight_set(0);
    expander_set_outputs(PIN_PWR_LCD | PIN_PWR_CODEC_PA, false);
    expander_set_outputs(PIN_PWR_SYSTEM, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    // Still powered: running from USB. Sleep with everything off; the expander's
    // interrupt (GPIO2, active low) wakes the chip on a dial press. Dropping the
    // rails changes its inputs and leaves the interrupt asserted until they are
    // read, which would wake the chip at once, so clear it first.
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_INPUT);
    gpio_pullup_en(GPIO_NUM_2);
    for (int attempt = 0; attempt < 20 && gpio_get_level(GPIO_NUM_2) == 0; attempt++) {
        uint16_t inputs;
        expander_read_inputs(&inputs);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (gpio_get_level(GPIO_NUM_2) == 0) {
        printf("PIPPOWER interrupt stuck low; staying awake with the screen off\n");
        return;
    }
    printf("PIPPOWER sleeping\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 0);
    rtc_gpio_pullup_en(GPIO_NUM_2);
    rtc_gpio_pulldown_dis(GPIO_NUM_2);
    esp_deep_sleep_start();
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    return pip_watcher_backlight_set(0);
}

esp_err_t pip_watcher_display_new(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_CLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = PIP_LCD_H_RES * 64 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "QSPI bus");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, io), TAG, "panel IO");

    spd2010_vendor_config_t vendor_config = {.flags = {.use_qspi_interface = 1}};
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,                     // no reset line; the driver uses a software reset
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(*io, &panel_config, panel), TAG, "SPD2010 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*panel, false, false), TAG, "panel orientation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "panel on");
    return ESP_OK;
}

esp_err_t pip_watcher_touch_new(esp_lcd_touch_handle_t *touch)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "touch I2C bus");

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    io_config.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io), TAG, "touch IO");
    const esp_lcd_touch_config_t touch_config = {
        .x_max = PIP_LCD_H_RES,
        .y_max = PIP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,      // shared with the LCD, which has none
        .int_gpio_num = GPIO_NUM_NC,      // interrupt is on the expander; LVGL polls
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_spd2010(io, &touch_config, touch), TAG, "SPD2010 touch");
    // Seeed reads once to wake the controller before registering it.
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(*touch);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static atomic_int knob_steps;
static knob_handle_t knob;

static void knob_turned(void *handle, void *data)
{
    (void)handle;
    atomic_fetch_add(&knob_steps, (int)(intptr_t)data);
    printf("PIPDIAL turn %+d\n", (int)(intptr_t)data);
}

static void knob_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->enc_diff = (int16_t)atomic_exchange(&knob_steps, 0);
    data->key = LV_KEY_ENTER;   // LVGL only treats presses with this key as the encoder button
    uint16_t inputs = 0;
    static bool pressed;
    // Pressed pulls the expander pin low. Keep the last state if the read fails.
    if (expander_read_inputs(&inputs) == ESP_OK) {
        const bool now = !(inputs & PIN_KNOB_BTN);
        if (now != pressed) printf("PIPDIAL button %s inputs=%04x\n", now ? "down" : "up", inputs);
        pressed = now;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t *pip_watcher_knob_add(lv_display_t *display)
{
    const knob_config_t config = {
        .default_direction = 0,
        .gpio_encoder_a = KNOB_A,
        .gpio_encoder_b = KNOB_B,
    };
    knob = iot_knob_create(&config);
    if (!knob) return NULL;
    // LVGL encoders treat positive diff as "next".
    if (iot_knob_register_cb(knob, KNOB_RIGHT, knob_turned, (void *)(intptr_t)1) != ESP_OK ||
        iot_knob_register_cb(knob, KNOB_LEFT, knob_turned, (void *)(intptr_t)-1) != ESP_OK) {
        return NULL;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, knob_read);
    lv_indev_set_display(indev, display);
    return indev;
}
