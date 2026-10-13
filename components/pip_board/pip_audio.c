#include "pip_board.h"
#include "pip_watcher.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stdatomic.h>

// Only the Rust audio task calls these functions. The display task never waits
// for codec I/O or the network. DAC/speaker output is not opened.
static const char *TAG = "pip_audio";
static esp_codec_dev_handle_t microphone;
static atomic_uint samples_read, peak_sample;

// Seeed Watcher: I2S0 shared by the ES7243(E) microphone ADC and the ES8311 DAC.
enum { I2S_MCLK = 10, I2S_SCLK = 11, I2S_LRCK = 12, I2S_DIN = 15, I2S_DOUT = 16 };
enum { ES7243_ADDRESS = 0x13, ES7243E_ADDRESS = 0x14 };

static esp_codec_dev_handle_t microphone_new(void)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.intr_priority = 4;
    // Seeed opens both directions so MCLK/BCLK/LRCK come from one full-duplex port.
    if (i2s_new_channel(&channel_config, &tx, &rx) != ESP_OK) return NULL;
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO, .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = 16, .ws_pol = false, .bit_shift = true, .left_align = true,
            .big_endian = false, .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK, .bclk = I2S_SCLK, .ws = I2S_LRCK, .dout = I2S_DOUT, .din = I2S_DIN,
        },
    };
    if (i2s_channel_init_std_mode(tx, &std_config) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) return NULL;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;   // the ADC drives the right slot
    if (i2s_channel_init_std_mode(rx, &std_config) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) return NULL;

    const audio_codec_i2s_cfg_t i2s_config = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_config);
    if (!data_if) return NULL;

    i2c_master_bus_handle_t bus = pip_watcher_i2c();
    const bool es7243 = i2c_master_probe(bus, ES7243_ADDRESS, 100) == ESP_OK;
    // esp_codec_dev takes the 8-bit (write) address.
    const audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0,
        .addr = (es7243 ? ES7243_ADDRESS : ES7243E_ADDRESS) << 1,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!control) return NULL;
    const audio_codec_if_t *codec;
    if (es7243) {
        es7243_codec_cfg_t config = {.ctrl_if = control};
        codec = es7243_codec_new(&config);
    } else {
        es7243e_codec_cfg_t config = {.ctrl_if = control};
        codec = es7243e_codec_new(&config);
    }
    ESP_LOGI(TAG, "microphone ADC: %s", es7243 ? "ES7243" : "ES7243E");
    if (!codec) return NULL;
    const esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if,
    };
    return esp_codec_dev_new(&device_config);
}

int32_t pip_audio_open(void)
{
    if (!microphone) microphone = microphone_new();
    if (!microphone) return ESP_FAIL;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = 16000, .channel = 2, .bits_per_sample = 16,
        // The ADC drives the right slot. With channel = 1 the codec layer would
        // reconfigure the receiver to the left slot and read silence. Still mono output.
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    };
    int result = esp_codec_dev_open(microphone, &format);
    if (result != ESP_OK) return result;
    // Seeed ships 27 dB; the host pipeline was tuned at 30 dB. Check peaks on device.
    result = esp_codec_dev_set_in_gain(microphone, 30.0f);
    if (result != ESP_OK) esp_codec_dev_close(microphone);
    atomic_store(&samples_read, 0);
    atomic_store(&peak_sample, 0);
    return result;
}

int32_t pip_audio_read(int16_t *samples, uint32_t count)
{
    if (!microphone || count != 320) return ESP_ERR_INVALID_ARG;
    int result = esp_codec_dev_read(microphone, samples, count * sizeof(int16_t));
    if (result == ESP_OK) {
        unsigned peak = atomic_load(&peak_sample);
        for (unsigned i = 0; i < count; i++) {
            unsigned value = samples[i] < 0 ? -(int)samples[i] : samples[i];
            if (value > peak) peak = value;
        }
        atomic_store(&peak_sample, peak);
        atomic_fetch_add(&samples_read, count);
    }
    return result;
}

void pip_audio_close(void)
{
    if (microphone) esp_codec_dev_close(microphone);
}

uint32_t pip_audio_samples(void) { return atomic_load(&samples_read); }
uint32_t pip_audio_peak(void) { return atomic_load(&peak_sample); }
