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
#include "audio_selftest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/gpio.h"
#include "hal/i2s_ll.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/lp_gpio_reg.h"
#include "soc/lp_iomux_reg.h"

#include "board_pins.h"
#include "bsp_common.h"
#include "bsp_usb.h"
#include "driver/gpio.h"

static const char *TAG = "BSP_AUDIO";
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static bool s_present;
static int s_init_error;      /* first failing step of bsp_audio_init() */
static uint32_t s_i2c_acks;   /* bit N set when 7-bit address N answered */
static uint8_t s_addr;
/* Runtime state. One place for every consumer (UI, shell, self test) to read;
 * no one reaches into the codec/I2S internals any more. */
static int  s_mclk_duty;                 /* last measured MCLK pad duty, % */
static bool s_pa_on;
static uint8_t s_volume = 50;            /* persisted by the app via NVS */
static uint8_t s_volume_reg = 0xBF;      /* last code written to 0x46/0x47 */
static bsp_audio_mic_route_t s_mic_route = {.left_sel = 6, .right_sel = 5,
                                            .gain = 4, .swap = false};

/* Snapshot of the MCLK pad registers BEFORE bsp_audio_init() repairs them,
 * so MIC PAD3 can show what the CPU-only reset left behind. */
static uint32_t s_mclk_pre_outsel;
static uint32_t s_mclk_pre_enable;
static uint32_t s_mclk_pre_lpiomux;

/* ES8389 可能的 7bit 地址：CE 引脚电平不同会变 */
/* Measured on this board: the ES8389 answers at 0x12 (CE strap), which the
 * first candidate list did not contain, so audio silently stayed disabled.
 * The board scan (acks=0x00040000) is what identified it. */
static const uint8_t s_addr_candidates[] = {
    0x12, ES8389_I2C_ADDR, 0x11, 0x20, 0x22,
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
    /* 0x02 bit[7:6] selects the codec's SYSTEM clock source:
     *   00 = MCLK pin (256*fs = 12.288 MHz from the P4)
     *   01 = BCLK pin (only 64*fs = 3.072 MHz here - far too slow for the
     *        ADC/DAC oversampling engines, so the chip stays digitally dead)
     * This table used to write 0x40 with a comment claiming the opposite;
     * the official esp_codec_dev driver (device/es8389/es8389.c) writes
     * 0x00 when a MCLK is present. Cross-checked against the Linux driver,
     * which also leaves ES8389_MASTER_CLK at 0x00. */
    {0x02, 0x00},              /* MASTER_CLK: system clock from MCLK pin */
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
    {0x2A, 0x00},              /* ADC misc (in official init table) */
    {0x2F, 0xC0},              /* ADC_MUTE */
    {0x30, 0xF4},              /* SYSTEM30 */
    {0x31, 0x00},              /* SYSTEM31 (in official init table) */
    {0x44, 0x00},              /* DAC misc (in official init table) */
    {0x41, 0x7F},              /* DAC_DSM_OSR */
    {0x42, 0x7F},              /* DAC_DSP_OSR */
    {0x43, 0x10},              /* DAC_MISC */
    {0x49, 0x0F},              /* DAC_RAMP */
    {0x4C, 0xC0},              /* SYSTEM4C */
    {0x00, 0x00},              /* release reset */
    {0x03, 0xC1},              /* CLK_OFF1 */
    {0x00, 0x01},
    {0x4D, 0x02},              /* DAC_RESET */
    {0x20, 0x63},              /* ADC format: S16_LE (3<<5) + mute bits */
    {0x40, 0x63},              /* DAC format: S16_LE (3<<5) + mute bits */
    /* 0x72/0x73 bits[6:4] pick the ADC input, bits[3:0] the PGA gain (3 dB/step):
     *   1 = differential, 5 = line P of that channel, 6 = line P of the other one
     * On this board ES_MIC1P and ES_MIC2N are crossed, so a differential pair
     * would compare two grounds (exact digital silence). Read each microphone
     * single-ended from the pin that actually carries its OUT instead:
     *   left  = 6 -> MIC2P (mic 2), right = 5 -> MIC2N (mic 1). */
    {0x72, 0x64},              /* ADCL: single MIC2P, PGA +12 dB */
    {0x73, 0x54},              /* ADCR: single MIC2N (mic 1), PGA +12 dB */
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
    /* Volume registers: 0xBF (=191) is the default code used by the official
     * esp_codec_dev driver. 0x00 is NOT "0 dB" - it is the bottom of the
     * attenuation range, i.e. DIGITAL SILENCE. Writing 0x00 here muted both
     * the DAC (speaker silent) and the ADC (mic stream constant zero) while
     * every clock looked perfect. */
    {0x26, 0xBF},              /* ADC volume */
    {0x27, 0xBF},              /* ADCL_VOL */
    {0x28, 0xBF},              /* ADCR_VOL */
    {0x46, 0xBF},              /* DACL_VOL */
    {0x47, 0xBF},              /* DACR_VOL */
    {0x48, 0xBE},              /* DAC mix volume */
    {0x40, 0x60},              /* DAC: S16_LE, unmuted */
    {0x20, 0x60},              /* ADC: S16_LE, unmuted (0x00 would mean 24-bit) */
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

/* ---------------- shared numeric helpers ----------------
 * A 512-entry Q15 sine and integer-only mixing replace the old
 * per-sample sin(double) in the streaming path. The LUT is built once at
 * init (where a little floating point is harmless). */
#define TONE_LUT_SIZE 512
static int16_t s_tone_lut[TONE_LUT_SIZE];
static bool s_tone_lut_ready;

static void tone_lut_init(void)
{
    if (s_tone_lut_ready) return;
    for (int i = 0; i < TONE_LUT_SIZE; ++i) {
        double a = 2.0 * 3.14159265358979 * (double)i / TONE_LUT_SIZE;
        s_tone_lut[i] = (int16_t)lround(32767.0 * sin(a));
    }
    s_tone_lut_ready = true;
}

void bsp_audio_fill_tone(int16_t *stereo, size_t frames, int freq_hz,
                         int16_t amplitude, uint32_t *phase_q16)
{
    if (!stereo || !phase_q16 || freq_hz <= 0) return;
    tone_lut_init();
    /* phase is Q16 over one cycle (0..65535); 2^32 is a whole number of
       cycles, so the natural uint32 wrap keeps the oscillator seamless. */
    const uint32_t step = (uint32_t)((uint64_t)(unsigned)freq_hz * 65536u /
                                     BSP_AUDIO_SAMPLE_RATE);
    uint32_t ph = *phase_q16;
    for (size_t i = 0; i < frames; ++i) {
        uint32_t idx = (ph >> 7) & (TONE_LUT_SIZE - 1);
        int32_t v = ((int32_t)s_tone_lut[idx] * (int32_t)amplitude) >> 15;
        stereo[2 * i]     = (int16_t)v;
        stereo[2 * i + 1] = (int16_t)v;
        ph += step;
    }
    *phase_q16 = ph;
}

/* Perceptual volume curve, integer only:
 *   attenuation_dB = 48 * (1 - p/100)^2
 *   0xBF = 0 dB, one register step = 0.5 dB, 0x00 = digital silence.
 * 50% therefore lands near -12 dB instead of the old -48 dB linear guess. */
static uint8_t volume_to_reg(int percent)
{
    if (percent <= 0) return 0x00;
    if (percent >= 100) return 0xBF;
    int q = 100 - percent;
    int atten = (96 * q * q) / 10000;       /* 0..96 half-dB steps */
    return (uint8_t)(0xBF - atten);
}

void bsp_audio_mic_route_default(bsp_audio_mic_route_t *route)
{
    if (!route) return;
    *route = (bsp_audio_mic_route_t){.left_sel = 6, .right_sel = 5,
                                     .gain = 4, .swap = false};
}

void bsp_audio_mic_route_get(bsp_audio_mic_route_t *route)
{
    if (route) *route = s_mic_route;
}

esp_err_t bsp_audio_mic_route_set(const bsp_audio_mic_route_t *route)
{
    if (!route || route->gain > 0x0E || route->left_sel > 7 || route->right_sel > 7)
        return ESP_ERR_INVALID_ARG;
    s_mic_route = *route;
    if (!s_present) return ESP_OK;   /* remembered, applied after init */
    esp_err_t e1 = codec_write(0x72, (uint8_t)((route->left_sel << 4) | route->gain));
    esp_err_t e2 = codec_write(0x73, (uint8_t)((route->right_sel << 4) | route->gain));
    return e1 != ESP_OK ? e1 : e2;
}

/* ---------------- ESP32-P4 MCLK pad source ----------------
 * The P4 feeds the MCLK output pad from one selectable module clock:
 * HP_SYS_CLKRST mst_clk_sel (1 = TX module, 0 = RX module), last write wins.
 * IDF v6.1's i2s_hal_set_rx_clock() flips it to the RX branch while the RX
 * channel is initialized. Both branches do output a waveform (measured
 * 45-54% duty on either setting), so this alone is not fatal - but with the
 * TX channel as the bus master, keep the pad on the TX clock after both
 * channels are up. */
static int mclk_pad_sel(void)
{
    return (int)HP_SYS_CLKRST.peri_clk_ctrl14.reg_i2s0_mst_clk_sel;
}

static void mclk_bind(int to_tx)
{
    PERIPH_RCC_ATOMIC() {
        if (to_tx) {
            i2s_ll_mclk_bind_to_tx_clk(I2S_LL_GET_HW(I2S_NUM_0));
        } else {
            i2s_ll_mclk_bind_to_rx_clk(I2S_LL_GET_HW(I2S_NUM_0));
        }
    }
}

/* GPIO3 is an LP/RTC pad on the P4. Three separate switches must all be
 * closed before the I2S0 MCLK signal can be observed on the pin:
 *   1. OUT_SEL = 26 (I2S0_MCLK_PAD_OUT_IDX)   - matrix routing
 *   2. OEN_SEL = 1 AND GPIO_ENABLE[3] = 1     - pad output driver
 *   3. LP_IOMUX PAD3.FUN_IE = 1               - pad input enable, needed
 *      only so gpio_get_level() can see the pin
 * esp_rom_gpio_connect_out_signal() sets (1) and GPIO_ENABLE but leaves
 * OEN_SEL = 0; the driver then stays owned by the I2S peripheral's idle OEN
 * line and the pin is dead even though OUT_SEL looks perfect. Likewise,
 * FUN_IE = 0 makes gpio_get_level() return 0 forever while the pin is very
 * much alive - every earlier "MCLK = 0%" reading was taken under that
 * condition and was misleading. Close all three here. */
static void mclk_pad_fix(void)
{
    esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, I2S0_MCLK_PAD_OUT_IDX, false, false);
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + PIN_I2S_MCLK * 4,
              (uint32_t)I2S0_MCLK_PAD_OUT_IDX | GPIO_FUNC0_OEN_SEL);
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(PIN_I2S_MCLK));
    /* Open the pad input so gpio_get_level() can observe the clock. Note this
     * is the HP IO_MUX input enable (gpio_input_enable), which does NOT touch
     * the output matrix - unlike gpio_set_direction(), which would rewrite
     * OUT_SEL to 256 and kill the routing we just restored. */
    gpio_input_enable(PIN_I2S_MCLK);
    REG_WRITE(LP_IOMUX_PAD3_REG,
              REG_READ(LP_IOMUX_PAD3_REG) | LP_IOMUX_REG_PAD3_FUN_IE);
}

/* Sample a pin 400 times and return the percentage read high. A running
   clock reads 30-60%, a stuck line 0% or 100%. */
static int gpio_duty(int pin)
{
    int high = 0;
    for (int k = 0; k < 400; ++k) high += gpio_get_level(pin) ? 1 : 0;
    return high * 100 / 400;
}

esp_err_t bsp_audio_init(void)
{
    /* 先关功放避免上电爆音，再给 CODEC_3V3 上电，等电源稳定。
       PA_CTRL 用输入+输出模式配置一次：纯输出模式下 gpio_get_level 永远读回 0，
       没法判断功放到底有没有被打开。 */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PIN_PA_CTRL,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_PA_CTRL, 0));
    /* A soft reset (SYS RESTART, or the CPU-only reset after flashing) does
       NOT reset peripherals: if the previous session died mid-I2C-transaction,
       the codec keeps holding SDA and the bus stays hung forever, and GPIO26
       stays high so the codec never resets itself (observed: register reads
       return 0xFF/timeout after a soft restart, while a cold boot is fine).
       Power-pulse the whole codec island so every init starts from a clean
       chip: 100 ms off, then 50 ms to stabilize before touching I2C. */
    bsp_gpio_out(PIN_AUDIO_PWR_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
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
    s_i2c_acks = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  codec bus ACK 0x%02X", addr);
            if (addr < 32) s_i2c_acks |= (1u << addr);
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
        s_init_error = ESP_ERR_NOT_FOUND;
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
    /* The register sequence can fail right after a soft restart (the bus or
       the codec can be in a half-hung state even after the power pulse; the
       RESET register writes mid-table also make the chip briefly deaf). One
       retry with a fresh power pulse recovers it; if it still fails, give up
       on the codec but keep going so the I2S buses still come up. */
    esp_err_t cfg_err = codec_write_table(s_es8389_init,
                            sizeof(s_es8389_init) / sizeof(s_es8389_init[0]));
    if (cfg_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        cfg_err = codec_write_table(s_es8389_on,
                    sizeof(s_es8389_on) / sizeof(s_es8389_on[0]));
    }
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "codec table write failed (0x%x), power-pulse and retry", cfg_err);
        bsp_gpio_out(PIN_AUDIO_PWR_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        bsp_gpio_out(PIN_AUDIO_PWR_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        cfg_err = codec_write_table(s_es8389_init,
                    sizeof(s_es8389_init) / sizeof(s_es8389_init[0]));
        if (cfg_err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            cfg_err = codec_write_table(s_es8389_on,
                        sizeof(s_es8389_on) / sizeof(s_es8389_on[0]));
        }
    }
    if (cfg_err == ESP_OK) {
        ESP_LOGI(TAG, "ES8389 initialized: slave I2S 48k/16bit, DAC+ADC on, PA muted");
    } else {
        /* A half-configured codec is useless but the I2S buses and the MIC/SPK
           debug commands should still exist, so log loudly and continue. */
        ESP_LOGE(TAG, "ES8389 register init failed twice (0x%x), audio will be silent", cfg_err);
    }
#else
    ESP_LOGW(TAG, "ES8389 register init disabled by CONFIG_APP_AUDIO_CODEC_INIT");
#endif

    /* I2S0 主模式全双工；MCLK 默认 256*fs = 12.288MHz，与 ES8389 CLK_MUL 配置一致 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) { s_init_error = (int)err; return err; }

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
    /* IDF v6.1 constitutes full-duplex only when BOTH channels declare the
     * same valid BCLK/WS pins - the official doc example passes one identical
     * gpio_cfg to both inits. Declaring them on the RX channel is not
     * "re-declaring": the demoted RX slave only adds input paths, the outputs
     * stay owned by TX. (An earlier attempt set them to I2S_GPIO_UNUSED here,
     * which silently degraded the pair into two simplex channels - the RX then
     * sampled DIN on its own free-running BCLK phase instead of the shared one.)
     *
     * MCLK history: the codec sat on digital zero for many builds because
     * GPIO3 never carried the I2S0 MCLK signal. Root cause: in IDF v6.1,
     * gpio_output_enable() - reached from gpio_set_direction() with ANY
     * output mode - first calls gpio_hal_matrix_out_default(), which
     * rewrites GPIO_FUNCn_OUT_SEL to 256 (plain GPIO) and silently
     * disconnects any peripheral signal previously routed to the pin. The
     * I2S driver itself never does this, but any later gpio_config() /
     * set_direction() touching the MCLK pin destroys the routing. Measured
     * with the MIC CLK diagnostic: out_sel=256 -> MCLK stuck low; restoring
     * out_sel=26 (I2S0_MCLK_PAD_OUT_IDX) brings the 12.288 MHz clock back
     * and the codec ADC immediately produces data. */
    if (err == ESP_OK && s_rx_chan) err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK && s_rx_chan) err = i2s_channel_enable(s_rx_chan);
    /* RX init left the MCLK pad clock source on the RX branch - see
     * mclk_pad_sel() above. Rebind to the TX master clock. */
    if (err == ESP_OK) mclk_bind(1);
    if (err == ESP_OK) {
        /* MCLK pad repair. Full causal chain, measured on this board:
         *   - the ES8389 takes its system clock from the MCLK pin only when
         *     reg 0x02 bit[7:6] = 00. This table used to write 0x40, which
         *     means "BCLK pin" - and BCLK here is only 64*fs = 3.072 MHz,
         *     far too slow for the ADC/DAC engines, so the codec stayed
         *     digitally dead no matter how good the I2S clocks looked;
         *   - with 0x02 = 0x00 the pin still stayed dead because the LP pad
         *     output driver was never enabled (OEN_SEL = 0 leaves it to the
         *     I2S peripheral's idle OEN line);
         *   - FUN_IE = 0 additionally made gpio_get_level() read 0, so the
         *     old "MCLK = 0%" measurements were partly an artefact.
         * mclk_pad_fix() closes all three switches. Run it after both I2S
         * channels are up so a reflash / soft restart cannot undo it. */
        s_mclk_pre_outsel  = REG_READ(GPIO_FUNC3_OUT_SEL_CFG_REG);
        s_mclk_pre_enable  = REG_READ(GPIO_ENABLE_REG);
        s_mclk_pre_lpiomux = REG_READ(LP_IOMUX_PAD3_REG);
        mclk_pad_fix();
        ESP_LOGI(TAG, "MCLK pad repaired: outsel %lu -> %lu, en_bit3 %lu -> %lu, duty=%d%%",
                 (unsigned long)(s_mclk_pre_outsel & 0x1FF),
                 (unsigned long)(REG_READ(GPIO_FUNC3_OUT_SEL_CFG_REG) & 0x1FF),
                 (unsigned long)((s_mclk_pre_enable >> PIN_I2S_MCLK) & 1u),
                 (unsigned long)((REG_READ(GPIO_ENABLE_REG) >> PIN_I2S_MCLK) & 1u),
                 gpio_duty(PIN_I2S_MCLK));
    }
    if (err == ESP_OK) {
        /* GPIO6 (DIN / ES8389 ASDOUT) must be a PLAIN INPUT.
         *
         * The CPU-only reset used after flashing does not reset the GPIO
         * matrix, so a stale pin configuration survives reboots. Observed
         * stale state on this board: GPIO_FUNC6_OUT_SEL = 28 with OEN_SEL = 0
         * (output enable taken from the peripheral). Signal index 28 on the
         * P4 is BOTH I2S0_O_SD_PAD_OUT_IDX (TX data out) and
         * I2S0_I_SD_PAD_IN_IDX (RX data in), and RX samples signal 28 from
         * GPIO6. The matrix therefore fed the outgoing TX samples straight
         * back into RX inside the chip: the "microphone" read a bit-exact
         * copy of the tone (rms 6363 = 9000/sqrt(2), peak 9000, L == R) even
         * with the ES8389 held in RESET and all its clocks off. That was
         * NEVER an acoustic loopback - the speaker/mic chain was never proven.
         *
         * Clearing sig_loopback alone does not help. Force OUT_SEL back to
         * SIG_GPIO_OUT_IDX (256) and disable the output driver so GPIO6 can
         * only carry the real ASDOUT signal into RX. */
        REG_WRITE(GPIO_FUNC6_OUT_SEL_CFG_REG, 256u);   /* SIG_GPIO_OUT_IDX */
        gpio_set_direction(PIN_I2S_DIN, GPIO_MODE_INPUT);
        ESP_LOGI(TAG, "GPIO6 forced to plain input: OUT_SEL=%lu en_bit6=%lu",
                 (unsigned long)(REG_READ(GPIO_FUNC6_OUT_SEL_CFG_REG) & 0x1FF),
                 (unsigned long)((REG_READ(GPIO_ENABLE_REG) >> PIN_I2S_DIN) & 1u));
    }
    if (err == ESP_OK) {
        tone_lut_init();
        /* The register table carries conservative defaults. Apply the runtime
           state last so a persisted calibration / volume always wins. */
        (void)bsp_audio_mic_route_set(&s_mic_route);
        (void)bsp_audio_set_volume(s_volume);
        s_mclk_duty = gpio_duty(PIN_I2S_MCLK);
        s_pa_on = gpio_get_level(PIN_PA_CTRL) != 0;
    }
    ESP_LOGI(TAG, "I2S0 48k/16bit stereo full-duplex: %s (mclk_pad=TX, mclk=%d%%, vol=%d, route=%u/%u)",
             err == ESP_OK ? "ready" : esp_err_to_name(err), s_mclk_duty,
             s_volume, s_mic_route.left_sel, s_mic_route.right_sel);
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

/* 0x20 / 0x40 hold BOTH the serial format (bits[7:2]) and the L/R mute bits
 * (bits[1:0]). Writing 0x00 or 0x03 there destroys S16_LE and silently
 * switches the codec to 24-bit - which is why an old "unmute" could leave the
 * stream dead. Always preserve 0x60 and only touch the two mute bits. */
#define ES8389_FMT_S16_I2S   0x60
#define ES8389_FMT_MUTE_BITS 0x03

esp_err_t bsp_audio_dac_mute(bool mute)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    return codec_write(0x40, ES8389_FMT_S16_I2S | (mute ? ES8389_FMT_MUTE_BITS : 0));
}

esp_err_t bsp_audio_adc_mute(bool mute)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    return codec_write(0x20, ES8389_FMT_S16_I2S | (mute ? ES8389_FMT_MUTE_BITS : 0));
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    esp_err_t e1 = bsp_audio_dac_mute(mute);
    esp_err_t e2 = bsp_audio_adc_mute(mute);
    return e1 != ESP_OK ? e1 : e2;
}

/* Frame-oriented primitives: one frame = one stereo sample pair. */
esp_err_t bsp_audio_tx_write(const int16_t *stereo, size_t frames, size_t *written_frames)
{
    if (!s_tx_chan || !stereo || !frames) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, stereo,
                                      frames * BSP_AUDIO_FRAME_BYTES, &bytes,
                                      pdMS_TO_TICKS(200));
    if (written_frames) *written_frames = bytes / BSP_AUDIO_FRAME_BYTES;
    return err;
}

esp_err_t bsp_audio_rx_read(int16_t *stereo, size_t frames, size_t *got_frames)
{
    if (!s_rx_chan || !stereo || !frames || !got_frames) return ESP_ERR_INVALID_ARG;
    size_t bytes = 0;
    *got_frames = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, stereo,
                                     frames * BSP_AUDIO_FRAME_BYTES, &bytes,
                                     pdMS_TO_TICKS(200));
    *got_frames = bytes / BSP_AUDIO_FRAME_BYTES;
    return err;
}

/* Legacy sample-oriented helpers kept for existing callers: `read` is in
 * BYTES (i2s_channel_read semantics), never samples. */
esp_err_t bsp_audio_play(const int16_t *samples, size_t len)
{
    return bsp_audio_tx_write(samples, len / 2, NULL);
}

esp_err_t bsp_audio_record(int16_t *samples, size_t len, size_t *read)
{
    size_t frames = 0;
    esp_err_t err = bsp_audio_rx_read(samples, len / 2, &frames);
    if (read) *read = frames * BSP_AUDIO_FRAME_BYTES;
    return err;
}

void bsp_audio_beep(uint32_t ms)
{
    if (!s_tx_chan) return;
    enum { N = 48 };
    int16_t buf[N * 2];
    uint32_t phase = 0;
    const int saved = bsp_audio_get_volume();
    (void)bsp_audio_set_volume(BSP_AUDIO_TEST_VOLUME_PCT);
    bsp_audio_fill_tone(buf, N, 1000, 8000, &phase);
    (void)bsp_audio_set_mute(false);
    (void)bsp_audio_speaker(true);
    uint32_t loops = ms / (N * 1000 / BSP_AUDIO_SAMPLE_RATE) + 1;
    for (uint32_t i = 0; i < loops; i++) {
        bsp_audio_fill_tone(buf, N, 1000, 8000, &phase);
        (void)bsp_audio_play(buf, N * 2);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    (void)bsp_audio_speaker(false);
    (void)bsp_audio_set_volume(saved);
}

bool bsp_audio_codec_present(void) { return s_present; }

/* ---------------- 麦克风电平 ---------------- */
#define MIC_BLOCK_FRAMES 480            /* 10 ms stereo @ 48 kHz */
static volatile int s_mic_rms[2], s_mic_peak[2];   /* L / R */
static volatile bool s_mic_running;
static volatile bool s_mic_pause;
static TaskHandle_t s_mic_task;

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t buf[MIC_BLOCK_FRAMES * 2];
    unsigned blocks = 0;
    for (;;) {
        if (s_mic_pause) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        size_t got = 0;   /* BYTES: i2s_channel_read reports bytes, not samples */
        esp_err_t err = bsp_audio_record(buf, MIC_BLOCK_FRAMES * 2, &got);
        size_t samples = got / sizeof(int16_t);
        if (err == ESP_OK && samples) {
            /* Interleaved stereo: even samples are the left ADC, odd the right.
               Two microphones therefore get two independent readings. */
            int64_t energy[2] = {0, 0};
            int peak[2] = {0, 0};
            size_t per_ch[2] = {0, 0};
            for (size_t i = 0; i < samples; ++i) {
                int ch = (int)(i & 1);
                int v = buf[i];
                if (v < 0) v = -v;
                energy[ch] += (int64_t)v * v;
                if (v > peak[ch]) peak[ch] = v;
                ++per_ch[ch];
            }
            for (int ch = 0; ch < 2; ++ch) {
                if (!per_ch[ch]) continue;
                int rms = (int)sqrt((double)energy[ch] / (double)per_ch[ch]);
                /* A 10 ms block is noisy frame to frame; follow rises fast and
                   decay slowly so the meter reads like a VU needle. */
                if (rms > s_mic_rms[ch]) s_mic_rms[ch] = rms;
                else s_mic_rms[ch] = s_mic_rms[ch] - (s_mic_rms[ch] - rms) / 4;
                s_mic_peak[ch] = peak[ch] > s_mic_peak[ch]
                    ? peak[ch] : s_mic_peak[ch] - s_mic_peak[ch] / 8;
            }
            if (++blocks == 200) { blocks = 0; }   /* keep the counter bounded */
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t bsp_audio_mic_start(void)
{
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;
    if (s_mic_task) return ESP_OK;
    if (xTaskCreate(mic_task, "mic_level", 4096, NULL, 3, &s_mic_task) != pdPASS) {
        s_mic_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_mic_running = true;
    ESP_LOGI(TAG, "mic capture started (I2S RX, 48 kHz stereo)");
    return ESP_OK;
}

void bsp_audio_mic_stop(void)
{
    if (!s_mic_task) return;
    TaskHandle_t task = s_mic_task;
    s_mic_task = NULL;
    s_mic_running = false;
    s_mic_rms[0] = s_mic_rms[1] = s_mic_peak[0] = s_mic_peak[1] = 0;
    vTaskDelete(task);
}

bool bsp_audio_mic_active(void) { return s_mic_running; }

esp_err_t bsp_audio_speaker(bool on)
{
    if (!s_tx_chan) return ESP_ERR_INVALID_STATE;
    ESP_ERROR_CHECK(gpio_set_level(PIN_PA_CTRL, on ? 1 : 0));
    s_pa_on = on;
    /* The amp needs a moment to leave shutdown; muting before it is ready is
       what makes the first milliseconds of playback disappear. */
    if (on) vTaskDelay(pdMS_TO_TICKS(25));
    return ESP_OK;
}

bool bsp_audio_speaker_on(void) { return s_pa_on; }

void bsp_audio_mic_pause(bool pause) { s_mic_pause = pause; }

#define TONE_BLOCK_FRAMES 480         /* 10 ms @ 48 kHz */

esp_err_t bsp_audio_tone(int freq_hz, int ms)
{
    if (!s_tx_chan || ms <= 0 || freq_hz <= 0) return ESP_ERR_INVALID_ARG;
    if (ms > 30000) ms = 30000;
    (void)bsp_audio_speaker(true);
    static int16_t buf[TONE_BLOCK_FRAMES * 2];
    uint32_t phase = 0;
    const size_t total = (size_t)BSP_AUDIO_SAMPLE_RATE * (size_t)ms / 1000;
    for (size_t done = 0; done < total; done += TONE_BLOCK_FRAMES) {
        bsp_audio_fill_tone(buf, TONE_BLOCK_FRAMES, freq_hz, 9000, &phase);
        (void)bsp_audio_tx_write(buf, TONE_BLOCK_FRAMES, NULL);
    }
    /* Fade to silence before releasing the amp, otherwise the shutdown click
       is louder than the tone. */
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 6; ++i)
        (void)bsp_audio_tx_write(buf, TONE_BLOCK_FRAMES, NULL);
    (void)bsp_audio_speaker(false);
    return ESP_OK;
}

esp_err_t bsp_audio_test_tone(int freq_hz, int ms)
{
    const int saved = bsp_audio_get_volume();
    esp_err_t e = bsp_audio_set_volume(BSP_AUDIO_TEST_VOLUME_PCT);
    esp_err_t t = bsp_audio_tone(freq_hz, ms);
    (void)bsp_audio_set_volume(saved);
    return e != ESP_OK ? e : t;
}

/* Payload packs (ms | pct<<16) so one worker can cover both the USB shell
 * (explicit volume) and the UI button (default 20%). */
static void tone_worker(void *arg)
{
    uint32_t p = (uint32_t)(uintptr_t)arg;
    int ms = (int)(p & 0xFFFFu);
    int pct = (int)((p >> 16) & 0xFFu);
    if (pct <= 0 || pct > 100) pct = BSP_AUDIO_TEST_VOLUME_PCT;
    const int saved = bsp_audio_get_volume();
    (void)bsp_audio_set_volume(pct);
    (void)bsp_audio_tone(1000, ms);
    (void)bsp_audio_set_volume(saved);
    vTaskDelete(NULL);
}

/* ---- global playback volume (state declared at the top of this file) ---- */

esp_err_t bsp_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = (uint8_t)percent;
    return bsp_audio_set_volume_raw(volume_to_reg(percent));
}

esp_err_t bsp_audio_set_volume_raw(uint8_t reg)
{
    if (reg > 0xBF) reg = 0xBF;
    s_volume_reg = reg;
    if (!s_present) return ESP_ERR_INVALID_STATE;
    /* De-duplicate: the slider can fire dozens of times per second and every
       write is a 100 kHz I2C transaction that the UI must not wait behind. */
    static uint8_t last = 0xFF;
    if (last == reg) return ESP_OK;
    esp_err_t e1 = codec_write(0x46, reg);
    esp_err_t e2 = codec_write(0x47, reg);
    if (e1 == ESP_OK && e2 == ESP_OK) last = reg;
    return e1 != ESP_OK ? e1 : e2;
}

uint8_t bsp_audio_volume_reg(void) { return s_volume_reg; }
int bsp_audio_get_volume(void) { return s_volume; }

void bsp_audio_mic_level(int rms[2], int peak[2])
{
    for (int ch = 0; ch < 2; ++ch) {
        if (rms) rms[ch] = s_mic_rms[ch];
        if (peak) peak[ch] = s_mic_peak[ch];
    }
}

void bsp_audio_status(bsp_audio_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->codec_present = s_present;
    out->codec_addr    = s_addr;
    out->init_error    = s_init_error;
    out->volume        = s_volume;
    out->volume_reg    = s_volume_reg;
    out->mclk_duty     = s_mclk_duty;
    out->pa_on         = s_pa_on;
    out->tx_ready      = s_tx_chan != NULL;
    out->rx_ready      = s_rx_chan != NULL;
    out->mic_running   = s_mic_running;
    out->i2c_acks      = s_i2c_acks;
    out->route         = s_mic_route;
}

bool bsp_audio_command(const char *line)
{
    if (line && strncmp(line, "AUDIO ", 6) == 0)
        return audio_selftest_command(line);
    if (line && strncmp(line, "SPK ", 4) == 0) {
        /* SPK ON | OFF | TONE <ms>: speaker enable and a hearable test tone. */
        char verb[8] = {0};
        int arg = 0;
        int got = sscanf(line, "SPK %7s %d", verb, &arg);
        if (got >= 1) {
            char out[96];
            if (strcmp(verb, "TONE") == 0) {
                if (arg <= 0 || arg > 3000) arg = 300;
                /* Optional third argument overrides the quiet test level;
                   used only by the volume-sweep regression. */
                int pct = BSP_AUDIO_TEST_VOLUME_PCT;
                if (sscanf(line, "SPK %*s %*d %d", &pct) != 1 || pct < 0 || pct > 100)
                    pct = BSP_AUDIO_TEST_VOLUME_PCT;
                uint32_t payload = (uint32_t)arg | ((uint32_t)pct << 16);
                bool ok = xTaskCreate(tone_worker, "tone", 4096,
                                      (void *)(uintptr_t)payload, 3, NULL) == pdPASS;
                snprintf(out, sizeof(out), "MEO:SPK op=TONE ms=%d vol=%d queued=%d\n",
                         arg, pct, ok);
            } else if (strcmp(verb, "VOL") == 0) {
                /* SPK VOL <pct> sets, bare SPK VOL queries. */
                esp_err_t e = got >= 2 ? bsp_audio_set_volume(arg) : ESP_OK;
                snprintf(out, sizeof(out), "MEO:SPK op=VOL pct=%d err=%d\n",
                         bsp_audio_get_volume(), (int)e);
            } else if (strcmp(verb, "ON") == 0 || strcmp(verb, "OFF") == 0) {
                esp_err_t e = bsp_audio_speaker(strcmp(verb, "ON") == 0);
                snprintf(out, sizeof(out), "MEO:SPK op=%s err=%d pa_gpio=%d\n",
                         verb, (int)e, gpio_get_level(PIN_PA_CTRL));
            } else {
                snprintf(out, sizeof(out), "MEO:SPK_ACCEPT op=STATUS pa_gpio=%d\n",
                         gpio_get_level(PIN_PA_CTRL));
            }
            (void)bsp_usb_cdc_write(out);
        }
        return true;
    }
    if (!line || strncmp(line, "MIC ", 4) != 0) return false;
    char verb[12] = {0}, token[17] = {0};
    int used = 0;
    if (sscanf(line, "MIC %11s %16s %n", verb, token, &used) != 2) return true;
    for (const char *p = token; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
            return true;
    char reply[160];
    if (strcmp(verb, "START") == 0) {
        esp_err_t err = bsp_audio_mic_start();
        snprintf(reply, sizeof(reply), "MEO:MIC token=%s op=START ok=%d error=%d\n",
                 token, err == ESP_OK, (int)err);
    } else if (strcmp(verb, "STOP") == 0) {
        bsp_audio_mic_stop();
        snprintf(reply, sizeof(reply), "MEO:MIC token=%s op=STOP ok=1\n", token);
    } else if (strcmp(verb, "LINE") == 0) {
        /* Is ASDOUT driven at all? Read the DIN pin with the internal pull-up,
           then pull-down. A driven line keeps its level; a floating one follows
           the pull. This separates "codec sends 0xFFFF" from "nobody drives it". */
        int as_is = gpio_get_level(PIN_I2S_DIN);
        gpio_set_pull_mode(PIN_I2S_DIN, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_down = gpio_get_level(PIN_I2S_DIN);
        gpio_set_pull_mode(PIN_I2S_DIN, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_up = gpio_get_level(PIN_I2S_DIN);
        gpio_set_pull_mode(PIN_I2S_DIN, GPIO_FLOATING);
        char out[128];
        snprintf(out, sizeof(out),
                 "MEO:MIC_LINE token=%s din=%d pulldown=%d pullup=%d -> %s\n",
                 token, as_is, with_down, with_up,
                 (with_down == with_up) ? "DRIVEN" : "FLOATING");
        (void)bsp_usb_cdc_write(out);
        return true;
    } else if (strcmp(verb, "RAW") == 0) {
        /* Dump the first samples of one RX block. All-zero means the line is
           held low, a constant value means DC, a repeating pattern means a
           format/clock mismatch, random means it is real noise. */
        /* len is in samples, the read count comes back in bytes: size the
           buffer for len samples and divide before printing (an earlier
           version printed 32 samples out of a 16-element buffer). */
        int16_t buf[64] = {0};
        size_t got = 0;
        /* The level task reads the same channel; two concurrent i2s_channel_read
           callers abort the driver. Stop it for the duration of the dump. */
        /* Ask the level task to stand down instead of deleting it: deleting a
           task that is blocked inside i2s_channel_read leaves the driver locked. */
        s_mic_pause = true;
        vTaskDelay(pdMS_TO_TICKS(40));
        esp_err_t e = bsp_audio_record(buf, 32, &got);
        size_t samples = got / sizeof(int16_t);
        if (samples > 32) samples = 32;
        char line[240];
        int n = snprintf(line, sizeof(line), "MEO:MIC_RAW token=%s bytes=%u n=%u err=%d ",
                         token, (unsigned)got, (unsigned)samples, (int)e);
        for (size_t i = 0; i < samples && i < 16 && n < (int)sizeof(line) - 8; ++i)
            n += snprintf(line + n, sizeof(line) - n, "%04x ", (uint16_t)buf[i]);
        snprintf(line + n, sizeof(line) - n, "\n");
        (void)bsp_usb_cdc_write(line);
        s_mic_pause = false;
        return true;
    } else if (strcmp(verb, "SEL") == 0) {
        /* MIC SEL <token> <left> <right> [gain]: left/right are the 0x72/0x73
           bits[6:4] codes (1 differential, 5 line-P, 6 other line-P), gain is
           the PGA step 0..14 (3 dB each). */
        unsigned lsel = 0, rsel = 0, gain = 0;
        if (sscanf(line, "MIC %*s %*s %u %u %u", &lsel, &rsel, &gain) >= 2) {
            if (gain > 0x0E) gain = 0x0E;
            bsp_audio_mic_route_t r = {.left_sel = (uint8_t)lsel,
                                       .right_sel = (uint8_t)rsel,
                                       .gain = (uint8_t)gain,
                                       .swap = s_mic_route.swap};
            esp_err_t e = bsp_audio_mic_route_set(&r);
            char out[128];
            snprintf(out, sizeof(out),
                     "MEO:MIC_SEL token=%s l=%u r=%u gain=%u err=%d\n",
                     token, lsel, rsel, gain, (int)e);
            (void)bsp_usb_cdc_write(out);
        }
        return true;
    } else if (strcmp(verb, "PAD3") == 0) {
        /* GPIO3 is an LP/RTC pad on the P4, so the pad has TWO owners:
         *   - LP_IOMUX PAD3.MUX_SEL  : 0 = digital GPIO matrix, 1 = LP GPIO
         *   - GPIO_FUNC3_OUT_SEL_CFG.OEN_SEL : 0 = peripheral OEN,
         *                                      1 = GPIO_ENABLE_REG[3]
         * A correct OUT_SEL=26 with a dead pad therefore means either the
         * output driver is disabled, or the pad is owned by the LP domain.
         *   MIC PAD3 <token> 0  read-only dump
         *   MIC PAD3 <token> 1  esp_rom_gpio_connect_out_signal (sig + OEN)
         *   MIC PAD3 <token> 2  GPIO_ENABLE_REG[3] = 0 (kill output)
         *   MIC PAD3 <token> 3  OUT_SEL=26|OEN_SEL=1 and GPIO_ENABLE[3]=1
         *   MIC PAD3 <token> 4  LP_IOMUX PAD3.MUX_SEL = 0 (digital GPIO)
         *   MIC PAD3 <token> 5  LP_IOMUX PAD3.MUX_SEL = 1 (LP GPIO)
         *   MIC PAD3 <token> 6  GPIO_ENABLE[3] = 1 only */
        int mode = 0;
        if (sscanf(line, "MIC %*s %*s %d", &mode) != 1) mode = 0;
        switch (mode) {
        case 1:
            esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, I2S0_MCLK_PAD_OUT_IDX, false, false);
            break;
        case 2:
            REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(PIN_I2S_MCLK));
            break;
        case 3:
            REG_WRITE(GPIO_FUNC3_OUT_SEL_CFG_REG,
                      (uint32_t)I2S0_MCLK_PAD_OUT_IDX | GPIO_FUNC3_OEN_SEL);
            REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(PIN_I2S_MCLK));
            break;
        case 4:
            REG_WRITE(LP_IOMUX_PAD3_REG,
                      REG_READ(LP_IOMUX_PAD3_REG) & ~LP_IOMUX_REG_PAD3_MUX_SEL_M);
            break;
        case 5:
            REG_WRITE(LP_IOMUX_PAD3_REG,
                      REG_READ(LP_IOMUX_PAD3_REG) | LP_IOMUX_REG_PAD3_MUX_SEL_M);
            break;
        case 6:
            REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(PIN_I2S_MCLK));
            break;
        default:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        uint32_t o3 = REG_READ(GPIO_FUNC3_OUT_SEL_CFG_REG);
        uint32_t en = REG_READ(GPIO_ENABLE_REG);
        uint32_t pad = REG_READ(LP_IOMUX_PAD3_REG);
        uint32_t lpen = REG_READ(LP_GPIO_ENABLE_REG);
        char out[320];
        snprintf(out, sizeof(out),
                 "MEO:MIC_PAD3 token=%s mode=%d outsel3=%lu oen_sel=%lu en3=%lu "
                 "duty3=%d%% lp_pad3=0x%08lx mux_sel=%lu fun_sel=%lu lp_en3=%lu "
                 "pre_outsel=%lu pre_en3=%lu pre_lp=0x%08lx\n",
                 token, mode,
                 (unsigned long)(o3 & 0x1FF),
                 (unsigned long)((o3 >> 10) & 1u),
                 (unsigned long)((en >> PIN_I2S_MCLK) & 1u),
                 gpio_duty(PIN_I2S_MCLK),
                 (unsigned long)pad,
                 (unsigned long)((pad & LP_IOMUX_REG_PAD3_MUX_SEL_M) ? 1 : 0),
                 (unsigned long)((pad & LP_IOMUX_REG_PAD3_FUN_SEL_M) >> LP_IOMUX_REG_PAD3_FUN_SEL_S),
                 (unsigned long)((lpen >> PIN_I2S_MCLK) & 1u),
                 (unsigned long)(s_mclk_pre_outsel & 0x1FF),
                 (unsigned long)((s_mclk_pre_enable >> PIN_I2S_MCLK) & 1u),
                 (unsigned long)s_mclk_pre_lpiomux);
        (void)bsp_usb_cdc_write(out);
        return true;
    } else if (strcmp(verb, "CLK") == 0) {
        /* Are the I2S clocks actually reaching the codec? Poll each line and
           report how often it reads high: a running clock gives a mixed ratio,
           a dead one gives 0% or 100%. MCLK additionally gets an A/B test of
           the P4 pad source select: bound to TX (running master clock) versus
           RX (the branch the v6.1 driver left it on, which never toggles). */
        const struct { const char *name; int pin; } lines[] = {
            {"MCLK", PIN_I2S_MCLK}, {"BCLK", PIN_I2S_BCLK}, {"LRCK", PIN_I2S_LRCK},
        };
        esp_err_t tx_err = i2s_channel_enable(s_tx_chan);
        int sel_before = mclk_pad_sel();
        mclk_bind(1);
        vTaskDelay(pdMS_TO_TICKS(2));
        int hi_tx = 0;
        for (int k = 0; k < 400; ++k) hi_tx += gpio_get_level(PIN_I2S_MCLK) ? 1 : 0;
        mclk_bind(0);
        vTaskDelay(pdMS_TO_TICKS(2));
        int hi_rx = 0;
        for (int k = 0; k < 400; ++k) hi_rx += gpio_get_level(PIN_I2S_MCLK) ? 1 : 0;
        mclk_bind(1);   /* leave the pad on the source that actually runs */
        /* Pad-level evidence. Read-only wherever possible: in IDF v6.1
           gpio_output_enable() (reached from gpio_set_direction with any
           OUTPUT mode) calls gpio_hal_matrix_out_default(), which REWRITES
           GPIO_FUNCn_OUT_SEL to 256 and silently disconnects any peripheral
           signal routed to the pin. So this section must never call
           gpio_set_direction(..., OUTPUT/INPUT_OUTPUT) on the MCLK pin after
           routing it - restoring is done with a bare matrix connect. */
        uint32_t o3 = REG_READ(GPIO_FUNC3_OUT_SEL_CFG_REG);
        int duty3 = gpio_duty(PIN_I2S_MCLK);
        int restored = 0;
        /* Compare only OUT_SEL[8:0]: the register also holds OEN_SEL in
           bit10, and comparing the raw word made this branch fire on every
           healthy pad and wipe OEN_SEL again. */
        if ((o3 & 0x1FF) != I2S0_MCLK_PAD_OUT_IDX) {
            mclk_pad_fix();
            restored = 1;
            o3 = REG_READ(GPIO_FUNC3_OUT_SEL_CFG_REG);
            duty3 = gpio_duty(PIN_I2S_MCLK);
        }
        /* Mirror the MCLK matrix signal onto spare header pin GPIO39: proves
           whether the I2S0 MCLK signal itself is toggling inside the chip. */
        uint32_t outsel39_saved = REG_READ(GPIO_FUNC39_OUT_SEL_CFG_REG);
        gpio_set_direction(GPIO_NUM_39, GPIO_MODE_INPUT);
        esp_rom_gpio_connect_out_signal(GPIO_NUM_39, I2S0_MCLK_PAD_OUT_IDX, false, 0);
        int duty_mirror = gpio_duty(GPIO_NUM_39);
        REG_WRITE(GPIO_FUNC39_OUT_SEL_CFG_REG, outsel39_saved);
        gpio_set_direction(GPIO_NUM_39, GPIO_MODE_INPUT);
        char ev[192];
        snprintf(ev, sizeof(ev),
                 "MEO:MIC_PAD outsel3=%lu(need %d) oen_sel=%lu duty3=%d%% restored=%d mirror39=%d%% "
                 "f4=%lu f5=%lu\n",
                 (unsigned long)(o3 & 0x1FF), (int)I2S0_MCLK_PAD_OUT_IDX,
                 (unsigned long)((o3 >> 10) & 1u),
                 duty3, restored, duty_mirror,
                 (unsigned long)REG_READ(GPIO_FUNC4_OUT_SEL_CFG_REG),
                 (unsigned long)REG_READ(GPIO_FUNC5_OUT_SEL_CFG_REG));
        (void)bsp_usb_cdc_write(ev);
        char out[256];
        int n = snprintf(out, sizeof(out),
                         "MEO:MIC_CLK token=%s tx_en=%d sel_before=%d mclk_tx=%d%% mclk_rx=%d%%",
                         token, (int)tx_err, sel_before, hi_tx * 100 / 400, hi_rx * 100 / 400);
        for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
            int high = 0;
            for (int k = 0; k < 400; ++k) high += gpio_get_level(lines[i].pin) ? 1 : 0;
            n += snprintf(out + n, sizeof(out) - n, " %s=%d%%", lines[i].name,
                          high * 100 / 400);
        }
        snprintf(out + n, sizeof(out) - n, "\n");
        (void)bsp_usb_cdc_write(out);
        return true;
    } else if (strcmp(verb, "LB") == 0) {
        /* ESP32-P4: i2s_ll_share_bck_ws() is implemented as
         * tx_conf.sig_loopback, and IDF sets it whenever it constitutes
         * full-duplex. On this chip that bit feeds the TX data straight into
         * the RX module: the "mic" then reads back a bit-exact copy of the
         * tone being played (rms 6363 = 9000/sqrt(2), peak 9000, L==R),
         * even with the codec held in RESET and its clocks off. Allow clearing
         * it at runtime to confirm the cause and restore real ASDOUT sampling.
         *
         * GPIO_FUNCn_IN_SEL_CFG_REG holds the GPIO number that drives
         * peripheral input signal n; I2S0_I_SD_PAD_IN_IDX must read 6. */
        int set = -1;
        if (sscanf(line, "MIC %*s %*s %d", &set) == 1) {
            PERIPH_RCC_ATOMIC() {
                I2S_LL_GET_HW(I2S_NUM_0)->tx_conf.sig_loopback = set ? 1 : 0;
            }
        }
        i2s_dev_t *hw = I2S_LL_GET_HW(I2S_NUM_0);
        uint32_t sd_in  = REG_READ(GPIO_FUNC1_IN_SEL_CFG_REG + (I2S0_I_SD_PAD_IN_IDX  - 1) * 4);
        uint32_t bck_in = REG_READ(GPIO_FUNC1_IN_SEL_CFG_REG + (I2S0_I_BCK_PAD_IN_IDX - 1) * 4);
        uint32_t ws_in  = REG_READ(GPIO_FUNC1_IN_SEL_CFG_REG + (I2S0_I_WS_PAD_IN_IDX  - 1) * 4);
        char out[224];
        snprintf(out, sizeof(out),
                 "MEO:MIC_LB token=%s loopback=%d sd_in=%lu(need %d) bck_in=%lu ws_in=%lu "
                 "out6=%lu out7=%lu\n",
                 token, (int)hw->tx_conf.sig_loopback,
                 (unsigned long)(sd_in & 0x3F), (int)PIN_I2S_DIN,
                 (unsigned long)(bck_in & 0x3F), (unsigned long)(ws_in & 0x3F),
                 (unsigned long)REG_READ(GPIO_FUNC6_OUT_SEL_CFG_REG),
                 (unsigned long)REG_READ(GPIO_FUNC7_OUT_SEL_CFG_REG));
        (void)bsp_usb_cdc_write(out);
        return true;
    } else if (strcmp(verb, "FIX") == 0) {
        /* P4 GPIO matrix: signal index 28 is BOTH I2S0_O_SD_PAD_OUT_IDX (TX
         * data out) and I2S0_I_SD_PAD_IN_IDX (RX data in). If GPIO6's OUT_SEL
         * carries 28 while RX samples signal 28 from GPIO6, the matrix feeds
         * the outgoing TX samples straight back into RX inside the chip - the
         * "mic" then reads a bit-exact copy of the tone. Restore GPIO6 to a
         * plain GPIO input (OUT_SEL = 256 = SIG_GPIO_OUT_IDX) so RX sees the
         * real ES8389 ASDOUT pin instead.
         *   MIC FIX <token> 1  -> restore plain input (default)
         *   MIC FIX <token> 0  -> put 28 back, for A/B comparison */
        int fix = 1;
        if (sscanf(line, "MIC %*s %*s %d", &fix) != 1) fix = 1;
        uint32_t before = REG_READ(GPIO_FUNC6_OUT_SEL_CFG_REG);
        REG_WRITE(GPIO_FUNC6_OUT_SEL_CFG_REG,
                  fix ? 256u : (uint32_t)I2S0_O_SD_PAD_OUT_IDX);
        gpio_set_direction(PIN_I2S_DIN, GPIO_MODE_INPUT);
        char out[160];
        snprintf(out, sizeof(out),
                 "MEO:MIC_FIX token=%s out6_before=%lu out6_after=%lu en=%lu\n",
                 token, (unsigned long)before,
                 (unsigned long)REG_READ(GPIO_FUNC6_OUT_SEL_CFG_REG),
                 (unsigned long)REG_READ(GPIO_ENABLE_REG));
        (void)bsp_usb_cdc_write(out);
        return true;
    } else if (strcmp(verb, "REG") == 0) {
        /* MIC REG <token> <reg> <val>: single write, for A/B experiments
           (PGA gain, ADC mute, input select) without a reflash. */
        unsigned reg = 0, val = 0;
        if (sscanf(line, "MIC %*s %*s %x %x", &reg, &val) == 2) {
            esp_err_t e = bsp_audio_write_reg((uint8_t)reg, (uint8_t)val);
            char out[96];
            snprintf(out, sizeof(out), "MEO:MIC_WR token=%s reg=%02X val=%02X err=%d\n",
                     token, reg, val, (int)e);
            (void)bsp_usb_cdc_write(out);
        }
        return true;
    } else if (strcmp(verb, "REGS") == 0) {
        /* Read back the registers the init table writes. If these do not match,
           the I2C writes never landed and every "codec configured" statement is
           fiction; if they match, the remaining problem is analog. */
        static const uint8_t regs[] = {0x00, 0x01, 0x02, 0x03, 0x10, 0x20, 0x23,
                                       0x2F, 0x64, 0x6D, 0x6F, 0x70, 0x72, 0x73,
                                       0xFD, 0xFE};
        char line[160];
        snprintf(line, sizeof(line), "MEO:MIC token=%s op=REGS addr=0x%02X present=%d\n",
                 token, s_addr, s_present);
        (void)bsp_usb_cdc_write(line);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            uint8_t v = 0xFF;
            esp_err_t e = bsp_audio_read_reg(regs[i], &v);
            snprintf(line, sizeof(line), "MEO:MIC_REG rx=%02X v=%02X err=%d\n",
                     regs[i], v, (int)e);
            (void)bsp_usb_cdc_write(line);
        }
        return true;
    } else if (strcmp(verb, "MUTE") == 0) {
        /* Decisive check for a floating analog input: muting the ADC must drop
           the meter to silence if the samples really come from the ADC. */
        int on = 0;
        if (sscanf(line + 4, "%*s %*s %d", &on) == 1)
            (void)bsp_audio_write_reg(0x20, on ? 0x03 : 0x00);   /* ADC_FORMAT_MUTE */
        snprintf(reply, sizeof(reply), "MEO:MIC token=%s op=MUTE adc_mute=%d\n", token, on);
    } else if (strcmp(verb, "STATUS") == 0) {
        int rms[2] = {0, 0}, peak[2] = {0, 0};
        bsp_audio_mic_level(rms, peak);
        /* dBFS with a floor: silence is reported as -96, never as -inf. */
        int db[2];
        for (int ch = 0; ch < 2; ++ch) {
            db[ch] = rms[ch] > 0 ? (int)(20 * log10((double)rms[ch] / 32768.0)) : -96;
            if (db[ch] < -96) db[ch] = -96;
        }
        snprintf(reply, sizeof(reply),
                 "MEO:MIC token=%s running=%d l_rms=%d r_rms=%d l_db=%d r_db=%d "
                 "l_peak=%d r_peak=%d codec=%d init_error=%d acks=0x%08lx rx=%d\n",
                 token, s_mic_running, rms[0], rms[1], db[0], db[1],
                 peak[0], peak[1], s_present, s_init_error,
                 (unsigned long)s_i2c_acks, s_rx_chan != NULL);
    } else {
        snprintf(reply, sizeof(reply), "MEO:MIC token=%s reason=syntax\n", token);
    }
    (void)bsp_usb_cdc_write(reply);
    return true;
}

