#include "pip_board.h"
#include "pip_assets.h"
#include "pip_sleep.h"
#include "pip_watcher.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"
#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(inter_tune);
LV_FONT_DECLARE(inter_tune_label);
LV_FONT_DECLARE(inter_tune_retry);
LV_FONT_DECLARE(inter_tune_exit);
LV_FONT_DECLARE(inter_tune_check);
LV_FONT_DECLARE(inter_55);
LV_FONT_DECLARE(inter_28);
LV_FONT_DECLARE(inter_40);
LV_FONT_DECLARE(inter_20);
LV_FONT_DECLARE(inter_choice);
LV_FONT_DECLARE(inter_confirm);
LV_FONT_DECLARE(inter_arrows);
LV_FONT_DECLARE(inter_check);
// 412x412 round panel (SenseCAP Watcher). Layouts keep content inside the circle:
// a text line at distance d from the centre can be at most 2*sqrt(206^2 - d^2) wide.
// Message text uses a 290 px column; its top line (d = 140 px) has 302 px available.
enum {
    WIDTH = 412, HEIGHT = 412, CENTER = 206, BLUE = 0x0028b9, RED = 0xa44200,
    TEXT_X = 61, TEXT_Y = 66, TEXT_W = 290,
    // Choice button: a circle, centred a little above the pagination row.
    CHOICE_D = 280, CHOICE_CY = 172, CHOICE_TEXT_W = 216, CHOICE_TEXT_H = 160,
    // Confirm button circle and the plain dismiss button.
    CONFIRM_D = 192, DISMISS_D = 128,
    // Pagination row (dots or A/B/C), pitch 36, at most 7 slots.
    ROW_CY = 332, ROW_SLOTS = 7,
    // Edge zones for touch paging, and the tuning layout's inscribed square.
    EDGE_L = 100, EDGE_R = 312,
};
// Tune review: Retry (top left), Exit (bottom left) and Submit (tall right) fill a
// 290 px square; the rounded outer corners stay inside the circle.
static const lv_area_t tune_boxes[3] = {
    {61, 61, 199, 199}, {61, 212, 199, 350}, {212, 61, 350, 350},
};
static lv_obj_t *scene;
static unsigned option_count;
static char option_labels[3][65];
static bool confirming;
static lv_point_t pressed;
static bool pointer_down;
// Retain ownership through completion/cancellation so releasing a hold cannot
// accidentally press a review button or start another recording.
static bool recording_hold;
static unsigned recording_hold_generation;
static bool attention_pressed;
static lv_timer_t *idle_timer;
static atomic_uint choice;
static atomic_uint interaction;
static atomic_uint audio_state;
static atomic_bool audio_started;
static lv_obj_t *tune_frame;
static atomic_uint tune_action;
static char tune_action_id[65];
static char tune_id[65], tune_phrase[161];
static char state[16] = "sleeping";
static char content[664];
static uint16_t offsets[168];
static unsigned pages, page;
static bool error_card, submitted;
static lv_group_t *dial_group;
static bool dial_recording;
static unsigned tune_focus;
static void render_page(void);
static void face(const char *name);

static void idle_activity(void)
{
    if (idle_timer && !strcmp(state, "idle")) lv_timer_reset(idle_timer);
}

static void idle_elapsed(lv_timer_t *timer)
{
    if (strcmp(state, "idle")) return;
    if (pointer_down) lv_timer_reset(timer);
    else face("sleeping");
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *mask(const lv_image_dsc_t *image, int x, int y, uint32_t color)
{
    lv_obj_t *obj = lv_image_create(scene);
    lv_image_set_src(obj, image);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_image_recolor(obj, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

// Place an image so its centre is dx, dy from the middle of the round screen.
static lv_obj_t *mask_center(const lv_image_dsc_t *image, int dx, int dy, uint32_t color)
{
    return mask(image, (WIDTH - (int)image->header.w) / 2 + dx, (HEIGHT - (int)image->header.h) / 2 + dy, color);
}

static void clear(uint32_t color)
{
    if (idle_timer) lv_timer_pause(idle_timer);
    pip_sleep_stop();
    tune_frame = NULL;
    lv_obj_clean(scene);
    lv_obj_set_style_bg_color(scene, lv_color_hex(color), 0);
}

static void dot(int x, int y, bool active, uint32_t background)
{
    lv_obj_t *obj = box(scene, x, y, 24, 24, active ? 0xffffff : background, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(obj, active ? 0 : 4, 0);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
}

// Figma trims text to cap height. LVGL positions the complete font line box.
static lv_obj_t *text_at(lv_obj_t *parent, const char *text, int x, int cap_y, int width,
    const lv_font_t *font, uint32_t color)
{
    lv_font_glyph_dsc_t glyph;
    lv_font_get_glyph_dsc(font, &glyph, 'H', 0);
    int inset = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 64 - font->line_height, 0);
    lv_obj_set_pos(label, x, cap_y - inset);
    lv_obj_set_width(label, width);
    lv_label_set_text(label, text);
    return label;
}

static void face(const char *name)
{
    pip_display_sleep(!strcmp(name, "sleeping"));
    uint32_t color = !strcmp(name, "listening") ? 0xff472a : !strcmp(name, "attention") ? 0xffd900 : 0;
    if (*tune_id) color = 0;
    clear(color);
    unsigned audio = atomic_load(&audio_state);
    atomic_store(&audio_started, false);
    atomic_store(&audio_state, !strcmp(name, "listening") ? ((audio & ~3u) + 4) | 1 : audio & ~3u);
    snprintf(state, sizeof(state), "%s", name);
    if (strcmp(name, "listening")) dial_recording = false;
    if (!strcmp(name, "idle")) {
        lv_timer_reset(idle_timer);
        lv_timer_resume(idle_timer);
    }
    if (*tune_id) {
        if (!strcmp(name, "tune_review") || !strcmp(name, "tune_wait")) {
            static const char *const labels[3] = {"Retry", "Exit", "Submit"};
            static const char *const icons[3] = {"↻", "×", "✓"};
            const lv_font_t *icon_fonts[3] = {&inter_tune_retry, &inter_tune_exit, &inter_tune_check};
            for (unsigned i = 0; i < 3; i++) {
                const lv_area_t *a = &tune_boxes[i];
                int w = lv_area_get_width(a), h = lv_area_get_height(a);
                lv_obj_t *button = box(scene, a->x1, a->y1, w, h, 0xffffff, 32);
                // The dial's focus ring; touch ignores it.
                if (i == tune_focus && !strcmp(name, "tune_review")) {
                    lv_obj_set_style_border_width(button, 8, 0);
                    lv_obj_set_style_border_color(button, lv_color_hex(0x2d6bff), 0);
                    lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
                }
                // Tall Submit box: icon and label sit around its vertical centre.
                int top = i == 2 ? a->y1 + h / 2 - 72 : a->y1 + 30;
                int label_y = i == 2 ? a->y1 + h / 2 + 14 : a->y1 + 84;
                lv_obj_t *icon = text_at(scene, icons[i], a->x1 + (w - 64) / 2, top, 64, icon_fonts[i], 0);
                lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_t *label = text_at(scene, labels[i], a->x1, label_y, w, &inter_tune_label, 0);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            }
        } else {
            lv_obj_t *label = text_at(scene, tune_phrase, TEXT_X, TEXT_Y, TEXT_W, &inter_tune, 0xffffff);
            lv_obj_set_style_text_line_space(label, 48 - inter_tune.line_height, 0);
            // Recording ring around the edge of the round screen, shown once audio starts.
            if (!strcmp(name, "listening")) {
                tune_frame = lv_obj_create(scene);
                lv_obj_remove_style_all(tune_frame);
                lv_obj_remove_flag(tune_frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_size(tune_frame, WIDTH, HEIGHT);
                lv_obj_set_pos(tune_frame, 0, 0);
                lv_obj_set_style_radius(tune_frame, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(tune_frame, 16, 0);
                lv_obj_set_style_border_color(tune_frame, lv_color_hex(0xff472a), 0);
                lv_obj_set_style_border_opa(tune_frame, LV_OPA_COVER, 0);
                lv_obj_add_flag(tune_frame, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (!strcmp(name, "attention")) {
        mask_center(&pip_face_attention, 0, 0, 0);
    } else if (!strcmp(name, "thinking")) {
        mask_center(&pip_face_thinking, 0, 0, 0xffffff);
        for (int i = 0; i < 3; i++) dot(152 + i * 36, 44, true, 0);
    } else if (!strcmp(name, "listening")) {
        mask_center(&pip_face_listening, 0, 0, 0xffffff);
        mask(&pip_ears, CENTER + 107, CENTER - 86, 0xffffff);
        mask(&pip_ears_left, CENTER - 168, CENTER - 86, 0xffffff);
    } else if (!strcmp(name, "sleeping")) {
        pip_sleep_start(scene);
    } else {
        mask_center(&pip_face_outline, 0, 0, 0xffffff);
        mask(&pip_eye_happy, CENTER - 64, CENTER - 44, 0xffffff);
        mask(&pip_eye_happy, CENTER + 12, CENTER - 44, 0xffffff);
        mask(&pip_smile, CENTER - 46, CENTER + 28, 0xffffff);
    }
}

static int cap_height(const lv_font_t *font)
{
    lv_font_glyph_dsc_t glyph;
    lv_font_get_glyph_dsc(font, &glyph, 'H', 0);
    return glyph.box_h;
}

static void indicators(uint32_t background)
{
    unsigned capacity = ROW_SLOTS - option_count;
    unsigned visible = pages < capacity ? pages : capacity;
    unsigned first = page > capacity / 2 ? page - capacity / 2 : 0;
    if (first + visible > pages) first = pages - visible;
    int x = (WIDTH - (int)(visible + option_count) * 36) / 2 - (option_count ? 2 : 0);
    for (unsigned i = first; i < first + visible; i++, x += 36) {
        if (option_count) mask(i == page ? &pip_page_full : &pip_page_empty, x + 6, ROW_CY - 12, 0xffffff);
        else dot(x + 6, ROW_CY - 12, i == page, background);
    }
    for (unsigned i = 0; i < option_count; i++, x += 36) {
        bool active = page == pages + i;
        if (active) box(scene, x, ROW_CY - 18, 36, 36, 0xffffff, LV_RADIUS_CIRCLE);
        char letter[] = {(char)('A' + i), 0};
        lv_obj_t *label = text_at(scene, letter, x, ROW_CY - cap_height(&inter_choice) / 2,
            36, &inter_choice, active ? background : 0xffffff);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void option_text(const char *text)
{
    const lv_font_t *fonts[] = {&inter_55, &inter_40, &inter_28, &inter_20};
    const lv_font_t *font = fonts[0];
    unsigned lines = 1;
    int line_height = 64;
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        font = fonts[i];
        line_height = i ? font->line_height : 64;
        unsigned offset = 0;
        lines = 0;
        while (text[offset]) {
            uint32_t length = lv_text_get_next_line(text + offset, font, 0, CHOICE_TEXT_W, NULL, LV_TEXT_FLAG_NONE);
            if (!length) break;
            offset += length;
            lines++;
        }
        if (!lines) lines = 1;
        if ((int)(lines - 1) * line_height + cap_height(font) <= CHOICE_TEXT_H) break;
    }
    int height = (int)(lines - 1) * line_height + cap_height(font);
    if (height > CHOICE_TEXT_H && strchr(text, '\n')) {
        // Excess explicit line breaks must not hide part of an option.
        char compact[65];
        snprintf(compact, sizeof(compact), "%s", text);
        for (char *p = compact; *p; p++) if (*p == '\n') *p = ' ';
        option_text(compact);
        return;
    }
    lv_obj_t *label = text_at(scene, text, CENTER - CHOICE_TEXT_W / 2, CHOICE_CY - height / 2, CHOICE_TEXT_W, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, line_height - font->line_height, 0);
}

static void render_page(void)
{
    pip_display_sleep(0);
    uint32_t color = error_card ? RED : BLUE;
    clear(color);
    snprintf(state, sizeof(state), "message");
    if (page < pages) {
        char text[664];
        size_t size = offsets[page + 1] - offsets[page];
        memcpy(text, content + offsets[page], size);
        while (size && (text[size - 1] == ' ' || text[size - 1] == '\n')) size--;
        text[size] = 0;
        text_at(scene, text, TEXT_X, TEXT_Y, TEXT_W, &inter_55, 0xffffff);
    } else if (option_count && !confirming) {
        snprintf(state, sizeof(state), "choice");
        box(scene, CENTER - CHOICE_D / 2, CHOICE_CY - CHOICE_D / 2, CHOICE_D, CHOICE_D, 0xffffff, LV_RADIUS_CIRCLE);
        option_text(option_labels[page - pages]);
    } else if (option_count) {
        snprintf(state, sizeof(state), "confirm");
        box(scene, CENTER - CONFIRM_D / 2, CHOICE_CY - CONFIRM_D / 2, CONFIRM_D, CONFIRM_D, 0xffffff, LV_RADIUS_CIRCLE);
        lv_obj_t *check = text_at(scene, "✓", CENTER - 32, CHOICE_CY - 42, 64, &inter_check, 0);
        lv_obj_set_style_text_align(check, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(check, 0, 0);
        lv_label_set_long_mode(check, LV_LABEL_LONG_CLIP);
        text_at(scene, "Confirm", CENTER - 42, CHOICE_CY + 34, 100, &inter_confirm, 0);
        lv_obj_t *left = text_at(scene, "←", 26, CHOICE_CY - 20, 60, &inter_arrows, 0xffffff);
        lv_obj_t *right = text_at(scene, "→", WIDTH - 86, CHOICE_CY - 20, 60, &inter_arrows, 0xffffff);
        lv_obj_set_style_text_opa(left, LV_OPA_50, 0);
        lv_obj_set_style_text_opa(right, LV_OPA_50, 0);
    } else {
        lv_obj_t *button = box(scene, CENTER - DISMISS_D / 2, CHOICE_CY - DISMISS_D / 2, DISMISS_D, DISMISS_D, 0xffffff, LV_RADIUS_CIRCLE);
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &inter_check, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_label_set_text(label, "✓");
        lv_obj_center(label);
    }
    indicators(color);
}

static bool showing_card(void)
{
    return !strcmp(state, "message") || !strcmp(state, "choice") || !strcmp(state, "confirm");
}

static void navigate(int direction)
{
    if (!showing_card() || submitted) return;
    unsigned last = pages + (option_count ? option_count - 1 : 0);
    if (direction < 0 && page) page--;
    else if (direction > 0 && page < last) page++;
    confirming = false;
    render_page();
}

static void submit(unsigned index)
{
    submitted = true;
    atomic_store(&choice, index + 1);
    face("thinking");
}

// 1 = Retry, 2 = Exit, 3 = Submit.
static void tune_activate(unsigned action)
{
    snprintf(tune_action_id, sizeof(tune_action_id), "%s", tune_id);
    atomic_store(&tune_action, action);
    if (action == 2) { *tune_id = 0; face("idle"); }
    else snprintf(state, sizeof(state), "tune_wait");
}

static void tap(int x, int y)
{
    idle_activity();
    if (*tune_id && !strcmp(state, "tune_review")) {
        for (unsigned i = 0; i < 3; i++) {
            const lv_area_t *a = &tune_boxes[i];
            if (x >= a->x1 && x <= a->x2 && y >= a->y1 && y <= a->y2) { tune_activate(i == 0 ? 1 : i == 1 ? 2 : 3); break; }
        }
    } else if (*tune_id && !strcmp(state, "tuning")) {
        face("listening");
        atomic_store(&interaction, 1);
    } else if (*tune_id && !strcmp(state, "listening")) {
        unsigned audio = atomic_load(&audio_state);
        face("thinking");
        atomic_store(&audio_state, (audio & ~3u) | 2);
        atomic_store(&interaction, 2);
    } else if (!strcmp(state, "attention") && pages) {
        render_page();
    } else if (showing_card() && !submitted) {
        if (option_count && page >= pages) {
            if (confirming) {
                int dx = x - CENTER, dy = y - CHOICE_CY;
                if (dx * dx + dy * dy <= (CONFIRM_D / 2) * (CONFIRM_D / 2)) submit(page - pages);
                else if (x < EDGE_L) navigate(-1);
                else if (x >= EDGE_R) navigate(1);
            } else if ((x - CENTER) * (x - CENTER) + (y - CHOICE_CY) * (y - CHOICE_CY) <= (CHOICE_D / 2) * (CHOICE_D / 2)) {
                confirming = true;
                render_page();
            } else if (x < EDGE_L) navigate(-1);
            else if (x >= EDGE_R) navigate(1);
        } else if (x < EDGE_L && page) navigate(-1);
        else if (x >= EDGE_R) navigate(1);
        else if (!option_count && page == pages && x >= EDGE_L + 2 && x < EDGE_R) submit(0);
    }
}

static void release(int x0, int y0, int x1, int y1)
{
    idle_activity();
    int dx = x1 - x0, dy = y1 - y0;
    if (abs(dx) >= 48 && abs(dx) > abs(dy)) navigate(dx < 0 ? 1 : -1);
    else if (abs(dx) < 24 && abs(dy) < 24) tap(x1, y1);
}

static void pointer_press(int x, int y)
{
    if (pointer_down) return;
    idle_activity();
    pressed = (lv_point_t){x, y};
    pointer_down = true;
    attention_pressed = !strcmp(state, "attention");
    // Without Wi-Fi the audio task fails at once, which only shows as a red flash.
    if ((!strcmp(state, "sleeping") || !strcmp(state, "idle")) && pip_network_ready()) {
        recording_hold = true;
        face("listening");
        recording_hold_generation = atomic_load(&audio_state) & ~3u;
        atomic_store(&interaction, 1);
    }
}

static void pointer_release(int x, int y, bool lost)
{
    if (!pointer_down) return;
    pointer_down = false;
    idle_activity();
    if (recording_hold) {
        recording_hold = false;
        // Even a moved/lost touch ends recording. Never interpret this release
        // as navigation or a button click on the newly displayed review page.
        if (atomic_load(&audio_state) == (recording_hold_generation | 1)) {
            face("thinking");
            atomic_store(&audio_state, recording_hold_generation | 2);
            atomic_store(&interaction, 2);
        }
    } else if (!lost && (strcmp(state, "attention") || attention_pressed)) {
        release(pressed.x, pressed.y, x, y);
    }
}

static void pointer_event(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_indev(event);
    // The dial's presses are delivered to the same object. They have no position;
    // treating them as touches turned every press into a tap at (0, 0).
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        pointer_press(point.x, point.y);
    } else if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        pointer_release(point.x, point.y, false);
    } else if (lv_event_get_code(event) == LV_EVENT_PRESS_LOST) {
        pointer_release(point.x, point.y, true);
    }
}

void pip_ui_init(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    scene = box(screen, 0, 0, WIDTH, HEIGHT, 0, LV_RADIUS_CIRCLE);
    // All children are inset within the circular background. Clipping the
    // children to the circle adds unnecessary offscreen compositing.
    lv_obj_add_flag(scene, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scene, pointer_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scene, pointer_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scene, pointer_event, LV_EVENT_PRESS_LOST, NULL);
    idle_timer = lv_timer_create(idle_elapsed, 10000, NULL);
    face("sleeping");
}

int32_t pip_ui_state(const char *name)
{
    if (strcmp(name, "sleeping") && strcmp(name, "idle") && strcmp(name, "thinking") &&
        strcmp(name, "listening") && strcmp(name, "attention")) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(1000)) return ESP_ERR_TIMEOUT;
    *tune_id = 0;
    face(name);
    lvgl_port_unlock();
    return ESP_OK;
}

int32_t pip_ui_card(const char *title, const char *body, const char *const *labels,
    uint32_t count, uint32_t kind)
{
    if (!title || !body || !labels || count < 1 || count > 3) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(1000)) return ESP_ERR_TIMEOUT;
    *tune_id = 0;
    snprintf(content, sizeof(content), "%s%s%s", title, *title ? "\n" : "", body);
    option_count = kind == 1 ? count : 0;
    for (unsigned i = 0; i < option_count; i++) snprintf(option_labels[i], sizeof(option_labels[i]), "%s", labels[i]);
    confirming = false;
    pages = page = 0;
    unsigned offset = 0;
    offsets[0] = 0;
    while (content[offset] && pages < 166) {
        for (int line = 0; line < 4 && content[offset]; line++) {
            offset += lv_text_get_next_line(content + offset, &inter_55, 0, TEXT_W, NULL, LV_TEXT_FLAG_NONE);
        }
        offsets[++pages] = offset;
    }
    error_card = kind == 2;
    submitted = false;
    atomic_store(&choice, 0);
    face("attention");
    // A touch already in progress belongs to the previous screen/card.
    attention_pressed = false;
    lvgl_port_unlock();
    return ESP_OK;
}

uint32_t pip_ui_choice(void) { return atomic_exchange(&choice, 0); }
uint32_t pip_ui_interaction(void) { return atomic_exchange(&interaction, 0); }

void pip_ui_tap(int32_t x, int32_t y)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || !lvgl_port_lock(1000)) return;
    pointer_press(x, y);
    pointer_release(x, y, false);
    lvgl_port_unlock();
}

void pip_ui_drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 < 0 || x0 >= WIDTH || x1 < 0 || x1 >= WIDTH ||
        y0 < 0 || y0 >= HEIGHT || y1 < 0 || y1 >= HEIGHT || !lvgl_port_lock(1000)) return;
    pointer_press(x0, y0);
    pointer_release(x1, y1, false);
    lvgl_port_unlock();
}

void pip_ui_pointer(int32_t x, int32_t y, uint32_t phase)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || phase > 2 || !lvgl_port_lock(1000)) return;
    if (phase == 1) pointer_press(x, y);
    else pointer_release(x, y, phase == 2);
    lvgl_port_unlock();
}

void pip_ui_inspect(void)
{
    if (!lvgl_port_lock(1000)) return;
    printf("PIPEVENT {\"type\":\"ui\",\"state\":\"%s\",\"page\":%u,\"pages\":%u",
        state, page, pages);
    if (!strcmp(state, "sleeping")) pip_sleep_inspect();
    printf(",\"brightness\":%u,\"tuning\":%s", (unsigned)pip_display_brightness(), *tune_id ? "true" : "false");
    printf(",\"audio\":{\"state\":%u,\"started\":%s,\"samples\":%u,\"peak\":%u}}\n",
        atomic_load(&audio_state), atomic_load(&audio_started) ? "true" : "false",
        (unsigned)pip_audio_samples(), (unsigned)pip_audio_peak());
    lvgl_port_unlock();
}

void pip_ui_sleep_debug(uint32_t repeat, int32_t seek_ms)
{
    if (!lvgl_port_lock(1000)) return;
    if (!strcmp(state, "sleeping")) pip_sleep_debug(repeat != 0, seek_ms);
    lvgl_port_unlock();
}

uint32_t pip_ui_audio_state(void) { return atomic_load(&audio_state); }

void pip_ui_audio_complete(uint32_t generation, uint32_t success)
{
    if (!lvgl_port_lock(1000)) return;
    unsigned audio = atomic_load(&audio_state);
    if ((audio & ~3u) == generation && (audio & 3) != 0 &&
        (!strcmp(state, "thinking") || !strcmp(state, "listening"))) face(*tune_id ? (success ? "tune_review" : "tuning") : "idle");
    lvgl_port_unlock();
}

void pip_ui_audio_finish(uint32_t generation)
{
    if (!lvgl_port_lock(1000)) return;
    if (atomic_load(&audio_state) == (generation | 1) && !strcmp(state, "listening")) {
        face("thinking");
        atomic_store(&audio_state, generation | 2);
    }
    lvgl_port_unlock();
}

int32_t pip_ui_tune(const char *id, const char *phrase)
{
    if (!id || !phrase || strlen(id) > 64 || strlen(phrase) > 160) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(1000)) return ESP_ERR_TIMEOUT;
    if (!*id && !*tune_id) { lvgl_port_unlock(); return ESP_OK; }
    snprintf(tune_id, sizeof(tune_id), "%s", id);
    snprintf(tune_phrase, sizeof(tune_phrase), "%s", phrase);
    face(*tune_id ? "tuning" : "idle");
    lvgl_port_unlock();
    return ESP_OK;
}

void pip_ui_audio_tuning(char *id, uint32_t capacity)
{
    if (!capacity) return;
    *id = 0;
    if (!lvgl_port_lock(1000)) return;
    snprintf(id, capacity, "%s", tune_id);
    lvgl_port_unlock();
}

void pip_ui_audio_started(uint32_t generation)
{
    if (!lvgl_port_lock(1000)) return;
    if (atomic_load(&audio_state) == (generation | 1)) {
        atomic_store(&audio_started, true);
        if (tune_frame) lv_obj_remove_flag(tune_frame, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

uint32_t pip_ui_tune_action(char *id, uint32_t capacity)
{
    if (!capacity || !lvgl_port_lock(1000)) return 0;
    uint32_t action = atomic_exchange(&tune_action, 0);
    snprintf(id, capacity, "%s", tune_action_id);
    lvgl_port_unlock();
    return action;
}

// Rotary dial. The scene is the only focusable object, and the group stays in
// edit mode so every detent arrives as LV_EVENT_KEY (left/right). A short press
// is LV_EVENT_SHORT_CLICKED; a long press on idle or sleeping starts recording and
// the next press stops it.
static void dial_stop_recording(void)
{
    dial_recording = false;
    if (strcmp(state, "listening")) return;
    unsigned audio = atomic_load(&audio_state);
    face("thinking");
    atomic_store(&audio_state, (audio & ~3u) | 2);
    atomic_store(&interaction, 2);
}

static void dial_turn(int direction)
{
    idle_activity();
    if (!strcmp(state, "sleeping")) face("idle");
    else if (*tune_id && !strcmp(state, "tune_review")) {
        tune_focus = (tune_focus + (direction > 0 ? 1 : 2)) % 3;
        face("tune_review");
    } else if (!strcmp(state, "attention") && pages) render_page();
    else if (showing_card()) navigate(direction);
}

static void dial_press(void)
{
    idle_activity();
    if (dial_recording) { dial_stop_recording(); return; }
    if (!strcmp(state, "sleeping")) { face("idle"); return; }
    if (*tune_id && !strcmp(state, "tune_review")) {
        tune_activate(tune_focus == 0 ? 1 : tune_focus == 1 ? 2 : 3);
    } else if (*tune_id && (!strcmp(state, "tuning") || !strcmp(state, "listening"))) {
        tap(0, 0);
    } else if (!strcmp(state, "attention") && pages) {
        render_page();
    } else if (showing_card() && !submitted) {
        if (option_count && page >= pages) {
            if (confirming) submit(page - pages);
            else { confirming = true; render_page(); }
        } else if (page >= pages && !option_count) submit(0);   // text pages: turn, do not press
    }
}

static void dial_long_press(void)
{
    if (dial_recording || *tune_id || !pip_network_ready() ||
        (strcmp(state, "sleeping") && strcmp(state, "idle"))) return;
    idle_activity();
    face("listening");
    dial_recording = true;
    atomic_store(&interaction, 1);
}

static void dial_log(const char *what)
{
    printf("PIPDIAL %s state=%s page=%u/%u confirming=%d submitted=%d recording=%d\n",
        what, state, page, pages, confirming, submitted, dial_recording);
}

// Pressing the knob also makes it tick; those turns must not cancel a confirm.
static bool dial_button_down;
static uint32_t dial_released_at;

static void dial_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    // A touch tap makes LVGL focus the scene, which leaves edit mode; without it the
    // dial's turns never become KEY events. Re-enter it on every event.
    if (dial_group) lv_group_set_editing(dial_group, true);
    // Touch taps reach the scene as clicks too; the touch handler already owns them.
    // KEY events only come from the dial, and their parameter is not an indev.
    if (code != LV_EVENT_KEY) {
        lv_indev_t *source = lv_event_get_indev(event);
        if (source && lv_indev_get_type(source) == LV_INDEV_TYPE_POINTER) return;
    }
    switch (code) {
    case LV_EVENT_KEY: {
        uint32_t key = lv_event_get_key(event);
        if (dial_button_down || lv_tick_elaps(dial_released_at) < 300) break;
        if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) dial_turn(1);
        else if (key == LV_KEY_LEFT || key == LV_KEY_UP) dial_turn(-1);
        break;
    }
    case LV_EVENT_SHORT_CLICKED: dial_log("short-click"); dial_press(); dial_log("  ->"); break;
    case LV_EVENT_LONG_PRESSED: dial_log("long-press"); dial_long_press(); dial_log("  ->"); break;
    case LV_EVENT_PRESSED: dial_button_down = true; dial_log("pressed"); break;
    case LV_EVENT_RELEASED: dial_button_down = false; dial_released_at = lv_tick_get(); dial_log("released"); break;
    default: break;
    }
}

void pip_ui_attach_dial(lv_indev_t *dial)
{
    if (!dial || dial_group) return;
    dial_group = lv_group_create();
    lv_group_add_obj(dial_group, scene);
    lv_group_set_editing(dial_group, true);
    lv_indev_set_group(dial, dial_group);
    lv_obj_add_event_cb(scene, dial_event, LV_EVENT_ALL, NULL);
}
