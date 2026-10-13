#include "pip_board.h"

#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_lcd_touch.h"
#include "pip_watcher.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lvgl_port.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "lvgl.h"

static const char *TAG = "pip";
static lv_display_t *ui_display;
static lv_indev_t *dial;
static uint16_t *screen_pixels;
static uint32_t screen_flushes;
enum { CONFIGURED_BRIGHTNESS = 80, SLEEP_BRIGHTNESS = 20 };
static uint32_t display_brightness = SLEEP_BRIGHTNESS;

// Call while holding the LVGL lock; the panel I/O serializes commands with DMA.
void pip_display_sleep(uint32_t sleeping)
{
    uint32_t percent = sleeping ? SLEEP_BRIGHTNESS : CONFIGURED_BRIGHTNESS;
    if (percent == display_brightness) return;
    esp_err_t result = pip_watcher_backlight_set(percent);
    if (result == ESP_OK) display_brightness = percent;
    else ESP_LOGE(TAG, "Brightness update failed: %s", esp_err_to_name(result));
}

uint32_t pip_display_brightness(void) { return display_brightness; }

typedef struct {
    int64_t started, elapsed, wait_started, waited, flush_started, flushing;
    uint32_t strips, pixels;
} render_stats_t;
static render_stats_t render_current, render_last;
static uint32_t render_frames;

static void measure_render(lv_event_t *event)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START:
        render_current = (render_stats_t){.started = now};
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(event);
        render_current.strips++;
        render_current.pixels += lv_area_get_size(area);
        render_current.flush_started = now;
        break;
    }
    case LV_EVENT_FLUSH_FINISH:
        render_current.flushing += now - render_current.flush_started;
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        render_current.wait_started = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        render_current.waited += now - render_current.wait_started;
        break;
    case LV_EVENT_REFR_READY:
        if (render_current.strips) {
            render_current.elapsed = now - render_current.started;
            render_last = render_current;
            render_frames++;
        }
        break;
    default:
        break;
    }
}

static void report_render(void)
{
    if (!lvgl_port_lock(1000)) return;
    printf("PIPRENDER {\"frames\":%lu,\"refresh_us\":%lld,\"wait_us\":%lld,"
        "\"flush_us\":%lld,\"strips\":%lu,\"pixels\":%lu}\n",
        (unsigned long)render_frames, (long long)render_last.elapsed,
        (long long)render_last.waited, (long long)render_last.flushing,
        (unsigned long)render_last.strips, (unsigned long)render_last.pixels);
    lvgl_port_unlock();
}
static lv_obj_t *choice_buttons[3];
static atomic_uint choice;
enum { UI_WIDTH = PIP_LCD_H_RES, UI_HEIGHT = PIP_LCD_V_RES };
LV_FONT_DECLARE(inter_28);
LV_FONT_DECLARE(inter_20);

static void capture_flush(lv_event_t *event)
{
    // LVGL 9.2 PARTIAL mode: active buffer is the exact upcoming flush, before
    // the port rotates and byte-swaps it for the wire. Preserve logical RGB565.
    const lv_area_t *area = lv_event_get_param(event);
    const lv_draw_buf_t *buffer = lv_display_get_buf_active(ui_display);
    const size_t row_bytes = lv_area_get_width(area) * sizeof(uint16_t);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(screen_pixels + y * UI_WIDTH + area->x1,
            buffer->data + (y - area->y1) * buffer->header.stride, row_bytes);
    }
    screen_flushes++;
}

static void round_area(lv_event_t *event)
{
    // The SPD2010 QSPI path needs x ranges that start on a multiple of 4 and
    // end on 4N+3 (Seeed's bsp_lvgl_rounder_cb). Rows are unconstrained.
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~3;
    area->x2 |= 3;
}

int32_t pip_board_init(void)
{
    ESP_RETURN_ON_ERROR(pip_watcher_power_init(), TAG, "board power");
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(pip_watcher_display_new(&panel, &io), TAG, "panel initialization");
    ESP_RETURN_ON_ERROR(pip_watcher_backlight_set(SLEEP_BRIGHTNESS), TAG, "brightness");
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(pip_watcher_touch_new(&touch), TAG, "touch initialization");

    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_stack = 6144;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_config), TAG, "LVGL initialization");
    // Create the display and build the first scene atomically.
    ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, TAG, "LVGL initialization lock");
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = PIP_LCD_H_RES * 32,
        .double_buffer = true,
        .hres = PIP_LCD_H_RES,
        .vres = PIP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    // QSPI panel: use the SPI display port. The screen is mounted unrotated.
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(display, ESP_ERR_NO_MEM, TAG, "display buffers");
    ui_display = display;
    lv_display_add_event_cb(display, measure_render, LV_EVENT_ALL, NULL);
    screen_pixels = heap_caps_calloc(UI_WIDTH * UI_HEIGHT, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(screen_pixels, ESP_ERR_NO_MEM, TAG, "screenshot buffer");
    lv_display_add_event_cb(display, capture_flush, LV_EVENT_FLUSH_START, NULL);
    const lvgl_port_touch_cfg_t touch_config = {
        .disp = display,
        .handle = touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_config), ESP_FAIL, TAG, "touch input");
    lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    // Dial: turn = next/previous, press = enter. pip_ui attaches it to a group.
    dial = pip_watcher_knob_add(display);
    if (!dial) ESP_LOGW(TAG, "dial input unavailable");
    lv_display_set_antialiasing(display, true);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    pip_ui_init(display);
    if (dial) pip_ui_attach_dial(dial);
    ESP_LOGI(TAG, "UI=%ldx%ld; round panel, native orientation",
        (long)lv_display_get_horizontal_resolution(display),
        (long)lv_display_get_vertical_resolution(display));
    lvgl_port_unlock();
    // The CH342 bridge carries UART0 (console). Take over stdin/stdout with the
    // interrupt-driven driver so reads are non-blocking and writes are buffered.
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_0, 4096, 8192, 0, NULL, 0), TAG, "console driver");
    uart_vfs_dev_use_driver(UART_NUM_0);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    return ESP_OK;
}

static bool screenshot_write(const char *data, size_t size)
{
    // Queue whole lines with backpressure. Console printf writes one byte at a
    // time and may silently drop bytes after its short TX timeout.
    return uart_write_bytes(UART_NUM_0, data, size) == (int)size;
}

static void send_screenshot(void)
{
    const size_t size = UI_WIDTH * UI_HEIGHT * sizeof(uint16_t);
    uint8_t *snapshot = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot || !lvgl_port_lock(1000)) {
        free(snapshot);
        puts("PIPERROR screenshot unavailable");
        return;
    }
    // Freeze only the copy, not the USB transfer. Animations can keep running.
    memcpy(snapshot, screen_pixels, size);
    uint32_t flushes = screen_flushes;
    int64_t timestamp = esp_timer_get_time() / 1000;
    lvgl_port_unlock();
    uint32_t crc = esp_rom_crc32_le(0, snapshot, size);
    char line[300];
    int length = snprintf(line, sizeof(line), "\nPIPSHOT 1 %u %u RGB565LE %u %08lx %lu %lld\n",
        UI_WIDTH, UI_HEIGHT, (unsigned)size, (unsigned long)crc,
        (unsigned long)flushes, (long long)timestamp);
    // Keep ordinary console logs between captures. LVGL rendering continues.
    flockfile(stdout);
    fflush(stdout);
    if (!screenshot_write(line, length)) {
        goto done;
    }
    // ASCII framing survives console newline conversion. CRC and row offsets
    // let the host reject truncated/corrupted captures instead of saving them.
    static const char hex[] = "0123456789abcdef";
    for (size_t offset = 0; offset < size; offset += 128) {
        size_t count = size - offset < 128 ? size - offset : 128;
        length = snprintf(line, sizeof(line), "PIPDATA %u ", (unsigned)offset);
        for (size_t i = 0; i < count; i++) {
            line[length++] = hex[snapshot[offset + i] >> 4];
            line[length++] = hex[snapshot[offset + i] & 15];
        }
        line[length++] = '\n';
        if (!screenshot_write(line, length)) {
            goto done;
        }
        // USB may stay writable continuously. Give IDLE0 CPU time so a large
        // capture cannot starve the task watchdog while the LVGL task runs.
        if ((offset / 128) % 8 == 7) {
            vTaskDelay(1);
        }
    }
    screenshot_write("PIPEND\n", 7);
done:
    funlockfile(stdout);
    free(snapshot);
}

const char *pip_debug_poll(void)
{
    static char command[4096];
    static size_t length;
    static bool overflow;
    char byte;
    while (read(STDIN_FILENO, &byte, 1) == 1) {
        if (byte == '\n' || byte == '\r') {
            command[length] = '\0';
            if (!overflow && strcmp(command, "screenshot") == 0) {
                send_screenshot();
            } else if (!overflow && strcmp(command, "power-off") == 0) {
                puts("PIPPOWER off");
                fflush(stdout);
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
                pip_watcher_power_off();
            } else if (!overflow && strcmp(command, "network-stats") == 0) {
                pip_network_inspect();
            } else if (!overflow && strcmp(command, "render-stats") == 0) {
                report_render();
            } else if (!overflow && length != 0) {
                length = 0;
                return command;
            }
            length = 0;
            overflow = false;
        } else if (length < sizeof(command) - 1) {
            command[length++] = byte;
        } else {
            overflow = true;
        }
    }
    return NULL;
}

static void option_clicked(lv_event_t *event)
{
    uint32_t index = (uintptr_t)lv_event_get_user_data(event);
    unsigned expected = 0;
    if (atomic_compare_exchange_strong(&choice, &expected, index + 1)) {
        for (unsigned i = 0; i < 3; i++) {
            if (choice_buttons[i]) {
                lv_obj_remove_flag(choice_buttons[i], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_bg_color(choice_buttons[i], lv_color_hex(i == index ? 0xb6caff : 0x242426), 0);
            }
        }
    }
}

int32_t pip_legacy_card(const char *title, const char *body, const char *const *options, uint32_t count)
{
    if (!title || !body || !options || count < 1 || count > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *screen = lv_display_get_screen_active(ui_display);
    lv_obj_clean(screen);
    memset(choice_buttons, 0, sizeof(choice_buttons));
    atomic_store(&choice, 0);

    lv_obj_t *heading = lv_label_create(screen);
    lv_obj_set_style_text_font(heading, &inter_28, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(0xf5f5f0), 0);
    lv_obj_set_pos(heading, 24, 20);
    lv_obj_set_size(heading, UI_WIDTH - 48, 70);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_label_set_text(heading, title);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 24, 100);
    lv_obj_set_size(content, UI_WIDTH - 48, 160);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *text = lv_label_create(content);
    lv_obj_set_width(text, UI_WIDTH - 56);
    lv_obj_set_style_text_font(text, &inter_20, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0xc8c8cc), 0);
    lv_label_set_text(text, body);

    int width = (UI_WIDTH - 48 - (count - 1) * 8) / count;
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *button = lv_button_create(screen);
        choice_buttons[i] = button;
        lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, 24 + i * (width + 8), UI_HEIGHT - 84);
        lv_obj_set_size(button, width, 60);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x343438), 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(button, option_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *text = lv_label_create(button);
        lv_obj_set_width(text, width - 12);
        lv_obj_set_style_text_font(text, &inter_20, 0);
        lv_obj_set_style_text_color(text, lv_color_hex(0xf5f5f0), 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(text, options[i]);
        lv_obj_center(text);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

uint32_t pip_legacy_choice(void)
{
    return atomic_exchange(&choice, 0);
}

void pip_print_diagnostics(void)
{
    ESP_LOGI(TAG, "uptime=%lld ms; PSRAM=%u bytes; internal free=%u; PSRAM free=%u",
        (long long)(esp_timer_get_time() / 1000), (unsigned)esp_psram_get_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
