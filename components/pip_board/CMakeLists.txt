idf_component_register(
    SRCS "pip_board.c" "pip_watcher.c" "pip_ui.c" "pip_sleep.c" "inter_tune.c" "inter_tune_label.c" "inter_tune_retry.c" "inter_tune_exit.c" "inter_tune_check.c" "pip_audio.c" "pip_assets.c" "inter_55.c" "inter_check.c" "inter_choice.c" "inter_confirm.c" "inter_arrows.c" "pip_network.c" "inter_20.c" "inter_28.c" "inter_40.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_codec_dev esp_lcd_spd2010 esp_lcd_touch_spd2010 knob button esp_lvgl_port esp_lcd_touch lvgl esp_timer esp_psram esp_driver_uart esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gpio esp_driver_ledc esp_hw_support esp_lcd esp_wifi esp_event esp_netif nvs_flash
)
# ESP-IDF's Xtensa headers require GNU inline assembly extensions to C17.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -O2)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
# The software rasterizer is CPU-bound; optimize its pixel loops for speed.
idf_component_get_property(pip_lvgl_target lvgl__lvgl COMPONENT_LIB)
target_compile_options(${pip_lvgl_target} PRIVATE -O2)
