#include "xl9555_keys.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "audio_hw.h"

static const char *TAG = "xl9555_keys";

#define XL9555_ADDR 0x20

/* XL9555 register addresses */
#define REG_INPUT0   0x00
#define REG_INPUT1   0x01
#define REG_OUTPUT0  0x02
#define REG_OUTPUT1  0x03
#define REG_CONFIG0  0x06
#define REG_CONFIG1  0x07

/* Bit positions for keys on port1 */
#define KEY0_BIT 7  /* IO1_7 */
#define KEY1_BIT 6  /* IO1_6 */
#define KEY2_BIT 5  /* IO1_5 */
#define KEY3_BIT 4  /* IO1_4 */

/* Port0 bit 2 drives SPK_EN (active low to enable speaker) on this board */
#define SPK_EN_BIT 2

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

static esp_err_t xl9555_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 1000);
}

esp_err_t xl9555_keys_init(void)
{
    i2c_master_bus_handle_t bus = audio_hw_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "audio I2C not ready");

    if (s_dev == NULL)
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = XL9555_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add xl9555");
    }

    /* Configure: port0 bit2 as output for speaker enable, others inputs; port1 all inputs */
    uint8_t cfg0 = 0xFF & ~(1u << SPK_EN_BIT); /* 0=output */
    ESP_RETURN_ON_ERROR(xl9555_write_reg(REG_CONFIG0, cfg0), TAG, "cfg0");
    ESP_RETURN_ON_ERROR(xl9555_write_reg(REG_CONFIG1, 0xFF), TAG, "cfg1");

    /* Drive speaker enable low to turn on amplifier */
    uint8_t out0 = (uint8_t)~(1u << SPK_EN_BIT);
    ESP_RETURN_ON_ERROR(xl9555_write_reg(REG_OUTPUT0, out0), TAG, "spk on");
    ESP_RETURN_ON_ERROR(xl9555_write_reg(REG_OUTPUT1, 0xFF), TAG, "out1");
    return ESP_OK;
}

uint8_t xl9555_keys_scan(uint8_t mode)
{
    static uint8_t key_up = 1;
    uint8_t keyval = XL9555_KEY_NONE;

    if (mode)
    {
        key_up = 1;
    }

    uint8_t in[2] = {0};
    if (s_dev == NULL || xl9555_read_regs(REG_INPUT0, in, sizeof(in)) != ESP_OK)
    {
        return XL9555_KEY_NONE;
    }

    /* Active low */
    bool k0 = !(in[1] & (1u << KEY0_BIT));
    bool k1 = !(in[1] & (1u << KEY1_BIT));
    bool k2 = !(in[1] & (1u << KEY2_BIT));
    bool k3 = !(in[1] & (1u << KEY3_BIT));

    if (key_up && (k0 || k1 || k2 || k3))
    {
        key_up = 0;
        if (k0) keyval = XL9555_KEY0;
        else if (k1) keyval = XL9555_KEY1;
        else if (k2) keyval = XL9555_KEY2;
        else if (k3) keyval = XL9555_KEY3;
    }
    else if (!k0 && !k1 && !k2 && !k3)
    {
        key_up = 1;
    }

    return keyval;
}
