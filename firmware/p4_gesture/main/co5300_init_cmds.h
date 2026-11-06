/*
 * CO5300 初始化命令表
 *
 * 来源：Osptek 官方 AM200MT460460FLS2 例程
 *   esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9 / main/co5300_init_cmds.h
 * 按本项目 MIPI-DSI 1-lane、RGB565、460x460 配置做了裁剪，命令与延时以原例程为准。
 * 原始命令表版权归 Osptek 所有；本文件仅作适配引用，不主张原创。
 */
#ifndef CO5300_INIT_CMDS_H
#define CO5300_INIT_CMDS_H

#include "esp_lcd_co5300.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== CO5300 初始化命令数组 ====================
static co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    // {0x35, (uint8_t[]){0x00}, 1, 0},
#if LV_COLOR_DEPTH == 16
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#elif LV_COLOR_DEPTH == 24
    {0x3A, (uint8_t[]){0x77}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x0A, 0x01, 0xD5}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xCB}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static const size_t lcd_init_cmds_size = sizeof(lcd_init_cmds) / sizeof(co5300_lcd_init_cmd_t);

#ifdef __cplusplus
}
#endif

#endif // CO5300_INIT_CMDS_H