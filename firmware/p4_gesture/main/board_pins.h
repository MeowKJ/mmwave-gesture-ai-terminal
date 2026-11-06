/*
 * board_pins.h -- 中控底板 V1.0（毫米波智能桌面手势终端）引脚表
 *
 * 依据：SCH_Schematic4_2026-11-06.pdf / Netlist_Schematic4_2026-11-06.enet
 * 主控：U1 = 鱼鹰光电 OspreyPi ESP32P4C5（网表器件名写 ESP32P4C6，
 *      同系列 88 pin 模组，引脚兼容；P4 + C5/C6 双芯）。
 *
 * 注意：模组脚号(1..88)与 ESP32-P4 GPIO 编号是两套体系，
 *      本文件统一用 GPIO 编号，模组脚号只写在注释里备查。
 *
 * USB（仅此一个 Type-C 口）：U1.48/49 = USB_DN/USB_DP，是 P4 的
 *      USB 2.0 High-Speed OTG **专用** PHY 引脚（由 VDD_USBPHY 供电），
 *      不是 GPIO24~27，也不是 USB-Serial/JTAG。乐鑫硬件设计指南：
 *      "使用 USB 2.0 高速 OTG 下载固件时，必须接专用 USB_DM / USB_DP"。
 *  - 应用态：TinyUSB HS port，复合 CDC-ACM + HID，VID 0x303A / PID 0x4005
 *  - ROM 下载态：同一对脚重新枚举为 Espressif USB-OTG CDC，
 *    VID 0x303A / PID 0x0012（= esptool ESP32P4ROM.IMAGE_CHIP_ID 18）
 *  - 进下载模式靠软件置 FORCE_DOWNLOAD_BOOT + 复位，不按 BOOT/EN
 *  - BOOT(GPIO35)/GPIO36 均有 10K 上拉，strapping 正常
 *  - eFuse DIS_USB_OTG_DOWNLOAD_MODE 必须保持 0（默认）
 *  - D3 = USBLC6-2P6 ESD 管，HS 信号建议换 <1pF 低容器件
 *
 * 电源 HOLD / 按键开机（Schematic4 POWER 页，已按网表逐点核对）：
 *      SW1(TS6611B3JTP, 1/3=GND) 按住 -> D1/D2(BAT54W) 把 Q1(AO3401A
 *      P-MOS) 栅极网络拉低 -> VSYS 接通 SYS_PWR_EN -> U10 出 3V3；
 *      同时 PWR_DET(GPIO30) 被拉低供 MCU 读取。
 *      PWR_HOLD(GPIO29) -> Q2(AO3400A N-MOS) 栅极；固件第一件事把它拉高，
 *      Q2 导通后接管 Q1 栅极，松手即保持供电（= 按了以后开机）。
 *      R10(470K) 上拉 Q1 栅极、R13(470K) 下拉 PWR_HOLD，因此：
 *        - 必须按住 SW1 直到 app_main 跑到 bsp_power_init()，不能点按即松手；
 *        - 插 USB 不会自动开机（VBUS 只到 U4 充电与 VBUS_ON 分压检测）；
 *        - 芯片复位时 GPIO29 变高阻 -> Q2 关断 -> 3V3 掉电，下载随之中断。
 *      硬件建议把 R13 改接 VBUS_ON 作为「无感下载」保险（详见 README）。
 *      软件关机（拉低 PWR_HOLD）硬件可行，但本版本刻意不实现。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"

/* ============ 电源、按键、USB 检测 ============ */
#define PIN_PWR_HOLD        GPIO_NUM_29   /* U1.56  Q2 -> Q1 锁存 VSYS，上电后必须立刻拉高 */
#define PIN_PWR_DET         GPIO_NUM_30   /* U1.57  电源键 SW1 检测（按下为低，R11 10K 上拉） */
#define PIN_VBUS_ON         GPIO_NUM_28   /* U1.55  VBUS 经 R34/R35 100K/100K 分压 -> 输入检测，切勿配成输出 */
#define PIN_BUCK_PG         GPIO_NUM_47   /* U1.75  3V3 buck power-good 输入 */
#define PIN_BAT_AD_EN       GPIO_NUM_46   /* U1.74  Q5 -> Q4，打开电池分压采样（R38 470K 默认下拉关断） */
#define PIN_BAT_AD          GPIO_NUM_49   /* U1.77  电池分压 ADC：R36 100K / R37 100K => VBAT/2 */
#define PIN_BOOT_BTN        GPIO_NUM_35   /* U1.62  BOOT strap，U8 按键 + R23 10K 上拉 */
#define PIN_GPIO36_STRAP    GPIO_NUM_36   /* U1.63  GPIO36 strap，R22 10K 上拉（别拉低） */
#define PIN_CHIP_PU         GPIO_NUM_NC   /* U1.87  CHIP_PU，仅 R14 10K 上拉 + U7 按键 + C34 */

/* ============ 显示屏 CN3（28P FPC，1-lane MIPI DSI，CO5300） ============ */
#define PIN_LCD_RST         GPIO_NUM_12   /* CN3.15 OLED_RST，低有效 */
#define PIN_LCD_TE          GPIO_NUM_13   /* CN3.16 OLED_TE，屏撕裂同步输出（输入） */
#define PIN_LCD_VCI_EN      GPIO_NUM_20   /* CN3.24 VCI_EN，屏供电使能，高有效
                                            * R21 10K 上拉到 3V3：GPIO 高阻时屏也默认供电 */
#define LCD_H_RES           460
#define LCD_V_RES           460
#define LCD_DSI_LANES       1             /* CN3.18/19 CLK，21/22 D0，只有 1 lane */
#define LCD_DPI_CLK_MHZ     16

/* ============ 触摸（与屏同一条 FPC） ============ */
#define PIN_TP_SDA          GPIO_NUM_8    /* CN3.5 */
#define PIN_TP_SCL          GPIO_NUM_9    /* CN3.6 */
#define PIN_TP_RST          GPIO_NUM_10   /* CN3.7，低有效 */
#define PIN_TP_INT          GPIO_NUM_11   /* CN3.8，触摸中断（低有效） */
#define TP_I2C_PORT         I2C_NUM_0
#define TP_I2C_ADDR         0x15          /* CST820 7bit 地址 */

/* ============ 音频：ES8389 + NS4150B ============ */
#define PIN_AUDIO_PWR_EN    GPIO_NUM_26   /* U1.53  U5(HE9073) CE -> CODEC_3V3 */
#define PIN_PA_CTRL         GPIO_NUM_24   /* U1.50  NS4150B CTRL，高有效 */
#define PIN_ES_I2C_SDA      GPIO_NUM_1    /* U1.16  经 BSS138 电平转换到 CODEC_3V3 */
#define PIN_ES_I2C_SCL      GPIO_NUM_2    /* U1.17 */
#define ES_I2C_PORT         I2C_NUM_1
#define ES8389_I2C_ADDR     0x10          /* 常见 7bit 地址，以实际扫描结果为准 */
#define PIN_I2S_MCLK        GPIO_NUM_3
#define PIN_I2S_BCLK        GPIO_NUM_4
#define PIN_I2S_LRCK        GPIO_NUM_5
#define PIN_I2S_DOUT        GPIO_NUM_6    /* P4 -> ES8389 */
#define PIN_I2S_DIN         GPIO_NUM_7    /* ES8389 -> P4 */

/* ============ 毫米波雷达（H1 排针） ============ */
#define PIN_RADAR_PWR_EN    GPIO_NUM_27   /* U1.54  RADAR_3V3 使能 */
#define PIN_RADAR_NRST      GPIO_NUM_48   /* U1.76  雷达复位，低有效 */
#define PIN_RADAR_UART_TX   GPIO_NUM_21   /* U1.30  UARTA_TX */
#define PIN_RADAR_UART_RX   GPIO_NUM_22   /* U1.31  UARTA_RX */
#define PIN_HOST_REQ        GPIO_NUM_23   /* U1.32  雷达 HOST_REQ */
#define PIN_RADAR_SOP1      GPIO_NUM_43   /* U1.71 */
#define PIN_RADAR_SOP0      GPIO_NUM_54   /* U1.82 */
#define PIN_RADAR_232_TX    GPIO_NUM_52   /* U1.80 */
#define PIN_RADAR_232_RX    GPIO_NUM_53   /* U1.81 */
#define RADAR_UART_PORT     UART_NUM_1
#define RADAR_UART_BAUD     115200

/* ============ 备用 SPI / UART0（H1 引出） ============ */
#define PIN_SPI_CLK         GPIO_NUM_39
#define PIN_SPI_MOSI        GPIO_NUM_40
#define PIN_SPI_MISO        GPIO_NUM_41
#define PIN_SPI_CS          GPIO_NUM_42
#define PIN_U0TXD           GPIO_NUM_37   /* U1.64，调试备用 */
#define PIN_U0RXD           GPIO_NUM_38   /* U1.65 */

/* ============ 供 BSP / 烧写脚本对齐的常量 ============ */
#define USB_ESPRESSIF_VID   0x303A
#define USB_APP_PID         0x4005        /* TinyUSB 默认 0x4000 | CDC(1) | HID(4) */
#define USB_ROM_PID         0x0012        /* ROM USB-OTG 下载口 = P4 IMAGE_CHIP_ID(18) */
#define BAT_ADC_DIV_RATIO   2             /* R36 100K / R37 100K */
