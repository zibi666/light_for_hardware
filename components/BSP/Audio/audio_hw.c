#include "audio_hw.h"

#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_hw";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_codec = NULL;
static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_std_config_t s_i2s_cfg;
static bool s_i2s_enabled = false;
static uint8_t s_volume = 20;

static esp_err_t audio_i2c_init(void)
{
    if (s_i2c_bus)
    {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = AUDIO_I2C_PORT,
        .scl_io_num = AUDIO_I2C_SCL,
        .sda_io_num = AUDIO_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };

    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t audio_codec_attach(void)
{
    if (s_codec)
    {
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AUDIO_CODEC_ADDR,
        .scl_speed_hz = AUDIO_I2C_CLK_HZ,
    };

    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_codec);
}

static esp_err_t codec_write(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not ready");
    return i2c_master_transmit(s_codec, payload, sizeof(payload), 1000);
}

static void es8388_i2s_cfg(uint8_t fmt, uint8_t len)
{
    fmt &= 0x03;
    len &= 0x07;
    codec_write(23, (fmt << 1) | (len << 3));
}

static void es8388_adda_cfg(uint8_t dacen, uint8_t adcen)
{
    uint8_t tempreg = 0;
    tempreg |= (!dacen) << 0;
    tempreg |= (!adcen) << 1;
    tempreg |= (!dacen) << 2;
    tempreg |= (!adcen) << 3;
    codec_write(0x02, tempreg);
}

static void es8388_output_cfg(uint8_t o1en, uint8_t o2en)
{
    uint8_t tempreg = 0;
    tempreg |= o1en * (3 << 4);
    tempreg |= o2en * (3 << 2);
    codec_write(0x04, tempreg);
}

static void es8388_input_cfg(uint8_t in)
{
    codec_write(0x0A, (5 * in) << 4);
}

static void es8388_hpvol_set(uint8_t volume)
{
    if (volume > 33)
    {
        volume = 33;
    }

    codec_write(0x2E, volume);
    codec_write(0x2F, volume);
}

static void es8388_spkvol_set(uint8_t volume)
{
    if (volume > 33)
    {
        volume = 33;
    }

    codec_write(0x30, volume);
    codec_write(0x31, volume);
}

static esp_err_t codec_init(void)
{
    ESP_RETURN_ON_ERROR(audio_i2c_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(audio_codec_attach(), TAG, "add codec failed");

    /* Basic reset and clock tree setup */
    ESP_RETURN_ON_ERROR(codec_write(0x00, 0x80), TAG, "reset fail");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(codec_write(0x00, 0x00), TAG, "wake fail");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(codec_write(0x01, 0x58), TAG, "set 0x01 first");
    ESP_RETURN_ON_ERROR(codec_write(0x01, 0x50), TAG, "set 0x01 second");
    ESP_RETURN_ON_ERROR(codec_write(0x02, 0xF3), TAG, "set 0x02 first");
    ESP_RETURN_ON_ERROR(codec_write(0x02, 0xF0), TAG, "set 0x02 second");

    ESP_RETURN_ON_ERROR(codec_write(0x03, 0x09), TAG, "mic bias");
    ESP_RETURN_ON_ERROR(codec_write(0x00, 0x06), TAG, "ref enable");
    ESP_RETURN_ON_ERROR(codec_write(0x04, 0x00), TAG, "dac pwr");
    ESP_RETURN_ON_ERROR(codec_write(0x08, 0x00), TAG, "mclk div");
    ESP_RETURN_ON_ERROR(codec_write(0x2B, 0x80), TAG, "sync lrck");

    ESP_RETURN_ON_ERROR(codec_write(0x09, 0x88), TAG, "pga gain");
    ESP_RETURN_ON_ERROR(codec_write(0x0C, 0x4C), TAG, "adc data sel");
    ESP_RETURN_ON_ERROR(codec_write(0x0D, 0x02), TAG, "adc ratio");
    ESP_RETURN_ON_ERROR(codec_write(0x10, 0x00), TAG, "adc vol L");
    ESP_RETURN_ON_ERROR(codec_write(0x11, 0x00), TAG, "adc vol R");

    ESP_RETURN_ON_ERROR(codec_write(0x17, 0x18), TAG, "dac bit width");
    ESP_RETURN_ON_ERROR(codec_write(0x18, 0x02), TAG, "dac ratio");
    ESP_RETURN_ON_ERROR(codec_write(0x1A, 0x00), TAG, "dac vol L");
    ESP_RETURN_ON_ERROR(codec_write(0x1B, 0x00), TAG, "dac vol R");
    ESP_RETURN_ON_ERROR(codec_write(0x27, 0xB8), TAG, "mix L");
    ESP_RETURN_ON_ERROR(codec_write(0x2A, 0xB8), TAG, "mix R");

    es8388_adda_cfg(1, 0);
    es8388_input_cfg(0);
    es8388_output_cfg(1, 1);
    es8388_hpvol_set(20);
    es8388_spkvol_set(20);

    return ESP_OK;
}

static esp_err_t audio_i2s_init(void)
{
    if (s_tx_handle)
    {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "new channel");

    s_i2s_cfg.clk_cfg = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(44100);
    s_i2s_cfg.clk_cfg.mclk_multiple = AUDIO_I2S_MCLK_MULT;
    s_i2s_cfg.slot_cfg = (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    s_i2s_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    s_i2s_cfg.gpio_cfg = (i2s_std_gpio_config_t){
        .mclk = AUDIO_I2S_MCLK,
        .bclk = AUDIO_I2S_BCLK,
        .ws = AUDIO_I2S_LRCK,
        .dout = AUDIO_I2S_DOUT,
        .din = AUDIO_I2S_DIN,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv = false,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &s_i2s_cfg), TAG, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable tx");
    s_i2s_enabled = true;
    return ESP_OK;
}

static i2s_data_bit_width_t bits_to_width(uint8_t bits)
{
    switch (bits)
    {
    case 24:
        return I2S_DATA_BIT_WIDTH_24BIT;
    case 32:
        return I2S_DATA_BIT_WIDTH_32BIT;
    default:
        return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

esp_err_t audio_hw_init(void)
{
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec init failed");
    return audio_i2s_init();
}

esp_err_t audio_hw_set_volume(uint8_t volume)
{
    if (volume > 33)
    {
        volume = 33;
    }

    s_volume = volume;
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_INVALID_STATE, TAG, "codec not ready");
    es8388_hpvol_set(volume);
    es8388_spkvol_set(volume);
    return ESP_OK;
}

uint8_t audio_hw_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_hw_configure(uint32_t sample_rate_hz, uint8_t bits_per_sample, uint8_t channels)
{
    ESP_RETURN_ON_FALSE(s_tx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "i2s not ready");

    i2s_data_bit_width_t width = bits_to_width(bits_per_sample);

    s_i2s_cfg.slot_cfg.data_bit_width = width;
    s_i2s_cfg.slot_cfg.ws_width = width;
    s_i2s_cfg.slot_cfg.slot_mode = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    s_i2s_cfg.slot_cfg.slot_mask = (channels == 1) ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
    s_i2s_cfg.clk_cfg.sample_rate_hz = sample_rate_hz;

    if (s_i2s_enabled)
    {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_handle), TAG, "disable for reconfig");
        s_i2s_enabled = false;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_tx_handle, &s_i2s_cfg.slot_cfg), TAG, "slot cfg");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_handle, &s_i2s_cfg.clk_cfg), TAG, "clk cfg");

    uint8_t len_cfg = 3; /* default 16bit */
    if (width == I2S_DATA_BIT_WIDTH_24BIT)
    {
        len_cfg = 0;
    }
    else if (width == I2S_DATA_BIT_WIDTH_32BIT)
    {
        len_cfg = 4;
    }
    es8388_i2s_cfg(0, len_cfg);

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable after cfg");
    s_i2s_enabled = true;
    return ESP_OK;
}

esp_err_t audio_hw_start(void)
{
    ESP_RETURN_ON_FALSE(s_tx_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "i2s not ready");
    es8388_adda_cfg(1, 0);
    es8388_output_cfg(1, 1);
    if (s_i2s_enabled)
    {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(s_tx_handle);
    if (err == ESP_OK)
    {
        s_i2s_enabled = true;
    }
    return err;
}

void audio_hw_stop(void)
{
    if (s_tx_handle && s_i2s_enabled)
    {
        i2s_channel_disable(s_tx_handle);
        s_i2s_enabled = false;
    }
    es8388_adda_cfg(0, 0);
}

size_t audio_hw_write(const uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    if (!s_tx_handle || !data || len == 0)
    {
        return 0;
    }

    size_t bytes_written = 0;
    if (i2s_channel_write(s_tx_handle, data, len, &bytes_written, timeout_ticks) != ESP_OK)
    {
        return 0;
    }
    return bytes_written;
}

void audio_hw_deinit(void)
{
    if (s_tx_handle)
    {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        s_i2s_enabled = false;
    }

    if (s_codec)
    {
        i2c_master_bus_rm_device(s_codec);
        s_codec = NULL;
    }

    if (s_i2c_bus)
    {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
}

i2c_master_bus_handle_t audio_hw_get_i2c_bus(void)
{
    return s_i2c_bus;
}
