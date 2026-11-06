/*
 * 音频 BSP —— ES8389 codec + NS4150B 功放
 *
 * 硬件：CODEC_3V3 由 AUDIO_PWR_EN(GPIO26) 控制，I2C 经 BSS138 电平转换，
 *       I2S0 主模式 48kHz/16bit 立体声全双工（MCLK=256fs=12.288MHz）。
 *
 * ES8389 初始化序列取自 Linux 内核官方驱动
 *   sound/soc/codecs/es8389.c（Everest Semi 提交）的 es8389_init() /
 *   set_bias_level(BIAS_ON) / unmute 三条路径，寄存器地址对应
 *   sound/soc/codecs/es8389.h。芯片工作在 slave 模式，时钟由 P4 提供。
 */
#include "bsp_audio.h"

#include <math.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "bsp_common.h"

static const char *TAG = "BSP_AUDIO";
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static bool s_present;
static uint8_t s_addr;

/* ES8389 可能的 7bit 地址：CE 引脚电平不同会变 */
static const uint8_t s_addr_candidates[] = {
    ES8389_I2C_ADDR, 0x11, 0x20, 0x22,
};

/* {reg, val} —— 官方驱动 es8389_init() 的完整写入序列 */
static const uint8_t s_es8389_init[][2] = {
    {0xF3, 0x56},              /* ISO_CTL */
    {0x00, 0x7E},              /* RESET */
    {0xF3, 0x38},
    {0x24, 0x64},              /* ADC_HPF1 */
    {0x25, 0x04},              /* ADC_HPF2 */
    {0x45, 0x03},              /* DAC_INV */
    {0x60, 0x2A},              /* VMID */
    {0x61, 0xC9},              /* ANA_CTL1 */
    {0x62, 0x4F},              /* ANA_VSEL */
    {0x63, 0x06},              /* ANA_CTL2 */
    {0x6B, 0x00},              /* LOW_POWER1 */
    {0x6D, 0x16},              /* DMIC_EN */
    {0x6E, 0xAA},              /* PGA_SW */
    {0x6F, 0x66},              /* MOD_SW1 */
    {0x70, 0x99},              /* MOD_SW2 */
    {0x23, 0x00},              /* ADC_MODE: TDM off, I2S */
    {0x10, 0xC4},              /* CSM_JUMP */
    {0x01, 0x08},              /* MASTER_MODE: slave */
    {0xF1, 0x00},              /* CSM_STATE1 */
    {0x12, 0x01}, {0x13, 0x01}, {0x14, 0x01}, {0x15, 0x01},
    {0x16, 0x35}, {0x17, 0x09}, {0x18, 0x91}, {0x19, 0x28},
    {0x1A, 0x01}, {0x1B, 0x01}, {0x1C, 0x11},
    {0xF0, 0x13},              /* CHIP_MISC */
    {0x02, 0x00},              /* MASTER_CLK: from MCLK pin */
    {0x04, 0x00},              /* CLK_DIV1 */
    {0x05, 0x10},              /* CLK_MUL */
    {0x06, 0x00},              /* CLK_MUX1 */
    {0x07, 0xC0},              /* CLK_MUX2 */
    {0x08, 0x00},              /* CLK_CTL1 */
    {0x09, 0xC0},              /* CLK_CTL2 */
    {0x0A, 0x80},              /* CLK_CTL3 */
    {0x0B, 0x04},              /* SCLK_DIV  -> 64fs BCLK */
    {0x0C, 0x01},              /* LRCK_DIV1 -> fs */
    {0x0D, 0x00},              /* LRCK_DIV2 */
    {0x0F, 0x10},              /* OSC_CLK */
    {0x21, 0x1F},              /* ADC_OSR */
    {0x22, 0x7F},              /* ADC_DSP */
    {0x2F, 0xC0},              /* ADC_MUTE */
    {0x30, 0xF4},              /* SYSTEM30 */
    {0x41, 0x7F},              /* DAC_DSM_OSR */
    {0x42, 0x7F},              /* DAC_DSP_OSR */
    {0x43, 0x10},              /* DAC_MISC */
    {0x49, 0x0F},              /* DAC_RAMP */
    {0x4C, 0xC0},              /* SYSTEM4C */
    {0x00, 0x00},              /* release reset */
    {0x03, 0xC1},              /* CLK_OFF1 */
    {0x00, 0x01},
    {0x4D, 0x02},              /* DAC_RESET */
    {0x20, 0x03},              /* ADC_FORMAT_MUTE: I2S 16bit + mute */
    {0x40, 0x03},              /* DAC_FORMAT_MUTE: I2S 16bit + mute */
    {0x72, 0x10},              /* MIC1_GAIN: default mic sel */
    {0x73, 0x10},              /* MIC2_GAIN */
};

/* 上电 + 解除静音（官方 set_bias_level(BIAS_ON) + unmute 路径） */
static const uint8_t s_es8389_on[][2] = {
    {0x69, 0x20},              /* HPSW: headphone switch on */
    {0x61, 0xD9},              /* ANA_CTL1: analog power on */
    {0x64, 0x8F},              /* ADC_EN */
    {0x10, 0xE4},              /* CSM_JUMP: state on */
    {0x00, 0x01},              /* RESET */
    {0x03, 0xC3},              /* CLK_OFF1 */
    {0x4D, 0x00},              /* DAC_RESET release */
    {0x46, 0x00},              /* DACL_VOL 0dB */
    {0x47, 0x00},              /* DACR_VOL 0dB */
    {0x27, 0x00},              /* ADCL_VOL 0dB */
    {0x28, 0x00},              /* ADCR_VOL 0dB */
    {0x40, 0x00},              /* DAC_FORMAT_MUTE: unmute */
    {0x20, 0x00},              /* ADC_FORMAT_MUTE: unmute */
};

static esp_err_t codec_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
}

static esp_err_t codec_write_table(const uint8_t table[][2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = codec_write(table[i][0], table[i][1]);
        ESP_RETURN_ON_ERROR(err, TAG, "write reg 0x%02X failed", table[i][0]);
    }
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    /* 先关功放避免上电爆音，再给 CODEC_3V3 上电，等电源稳定 */
    bsp_gpio_out(PIN_PA_CTRL, 0);
    bsp_gpio_out(PIN_AUDIO_PWR_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ES_I2C_PORT,
        .sda_io_num = PIN_ES_I2C_SDA,
        .scl_io_num = PIN_ES_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    /* 扫一遍这条总线，把实际 ACK 的地址都打出来（便于核对 BSS138 与 CE 电平） */
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  codec bus ACK 0x%02X", addr);
        }
    }

    /* 只有真正 probe 到 codec 才认为存在；i2c_master_bus_add_device() 本身
       永远返回成功，不能拿它当探测结果。 */
    bool found = false;
    for (size_t i = 0; i < sizeof(s_addr_candidates) / sizeof(s_addr_candidates[0]); i++) {
        if (i2c_master_probe(bus, s_addr_candidates[i], 50) == ESP_OK) {
            s_addr = s_addr_candidates[i];
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "no ES8389 ACK on I2C%d (SDA=%d SCL=%d); audio disabled",
                 ES_I2C_PORT, PIN_ES_I2C_SDA, PIN_ES_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_codec), TAG, "add device");
    s_present = true;

    /* 读 CHIP_ID0/1 (0xFD/0xFE) 只作为日志，读不到不判失败 */
    uint8_t id0 = 0, id1 = 0;
    esp_err_t id_err = bsp_audio_read_reg(0xFD, &id0);
    if (id_err == ESP_OK) id_err = bsp_audio_read_reg(0xFE, &id1);
    ESP_LOGI(TAG, "ES8389 @0x%02X ACK, CHIP_ID=%02X %02X (%s)",
             s_addr, id0, id1, id_err == ESP_OK ? "ok" : "read failed");

#if CONFIG_APP_AUDIO_CODEC_INIT
    ESP_RETURN_ON_ERROR(codec_write_table(s_es8389_init,
                        sizeof(s_es8389_init) / sizeof(s_es8389_init[0])),
                        TAG, "codec init table");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(codec_write_table(s_es8389_on,
                        sizeof(s_es8389_on) / sizeof(s_es8389_on[0])),
                        TAG, "codec power-on table");
    ESP_LOGI(TAG, "ES8389 initialized: slave I2S 48k/16bit, DAC+ADC on, PA muted");
#else
    ESP_LOGW(TAG, "ES8389 register init disabled by CONFIG_APP_AUDIO_CODEC_INIT");
#endif

    /* I2S0 主模式全双工；MCLK 默认 256*fs = 12.288MHz，与 ES8389 CLK_MUL 配置一致 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) return err;

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT, .din = PIN_I2S_DIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err == ESP_OK && s_rx_chan) err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK && s_rx_chan) err = i2s_channel_enable(s_rx_chan);
    ESP_LOGI(TAG, "I2S0 48k/16bit stereo: %s", err == ESP_OK ? "ready" : esp_err_to_name(err));
    return err;
}

esp_err_t bsp_audio_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    return codec_write(reg, val);
}

esp_err_t bsp_audio_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_present || !val) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(s_codec, &reg, 1, val, 1, 100);
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    esp_err_t err = codec_write(0x40, mute ? 0x03 : 0x00);   /* DAC_FORMAT_MUTE */
    if (err == ESP_OK) err = codec_write(0x20, mute ? 0x03 : 0x00);  /* ADC_FORMAT_MUTE */
    return err;
}

esp_err_t bsp_audio_play(const int16_t *samples, size_t len)
{
    if (!s_tx_chan || !samples || !len) return ESP_ERR_INVALID_ARG;
    size_t written = 0;
    return i2s_channel_write(s_tx_chan, samples, len * sizeof(int16_t), &written,
                             pdMS_TO_TICKS(200));
}

esp_err_t bsp_audio_record(int16_t *samples, size_t len, size_t *read)
{
    if (!s_rx_chan || !samples || !len || !read) return ESP_ERR_INVALID_ARG;
    return i2s_channel_read(s_rx_chan, samples, len * sizeof(int16_t), read,
                            pdMS_TO_TICKS(200));
}

void bsp_audio_beep(uint32_t ms)
{
    if (!s_tx_chan) return;
    enum { N = 48, RATE = 48000 };
    int16_t buf[N * 2];
    for (int i = 0; i < N; i++) {
        int16_t v = (int16_t)(8000 * sin(2 * 3.14159265 * 1000.0 * i / RATE));
        buf[i * 2] = v; buf[i * 2 + 1] = v;
    }
    bsp_audio_set_mute(false);
    bsp_gpio_out(PIN_PA_CTRL, 1);
    uint32_t loops = ms / (N * 1000 / RATE) + 1;
    for (uint32_t i = 0; i < loops; i++) bsp_audio_play(buf, N * 2);
    vTaskDelay(pdMS_TO_TICKS(20));
    bsp_gpio_out(PIN_PA_CTRL, 0);
}

bool bsp_audio_codec_present(void) { return s_present; }
