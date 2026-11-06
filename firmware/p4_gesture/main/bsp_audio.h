#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ES8389 + NS4150B：上电、I2C 探测、官方寄存器初始化表、I2S0 48k/16bit 双向 */
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_write_reg(uint8_t reg, uint8_t val);
esp_err_t bsp_audio_read_reg(uint8_t reg, uint8_t *val);
esp_err_t bsp_audio_set_mute(bool mute);
esp_err_t bsp_audio_play(const int16_t *samples, size_t len);
esp_err_t bsp_audio_record(int16_t *samples, size_t len, size_t *read);
void bsp_audio_beep(uint32_t ms);
bool bsp_audio_codec_present(void);
