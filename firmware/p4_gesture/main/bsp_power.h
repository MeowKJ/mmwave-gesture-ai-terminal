#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 必须在 app_main 第一行调用：拉高 PWR_HOLD 锁住 VSYS，并释放上一次
   下载复位留下的 pad hold。 */
void bsp_power_init(void);

int  bsp_power_battery_mv(void);       /* 失败返回 -1 */
bool bsp_power_usb_attached(void);      /* VBUS 分压检测 */
bool bsp_power_key_pressed(void);       /* 电源键按下（低有效） */
bool bsp_power_buck_ok(void);

/* ---------- 无感下载专用的电源保持 ----------
   HOLD 电路：PWR_HOLD(GPIO29) -> Q2 -> Q1 -> 3V3；R13 470K 把 Q2 栅极
   下拉到 GND。芯片复位时 GPIO 变高阻，Q2 关断，3V3 会掉电，烧写随之中断。
   P4 rev >= 3.0 的 pad hold 寄存器位于 LP_SYS 常开域
   (LP_SYSTEM_REG_PAD_RTC_HOLD_CTRL0_REG)，能在 TOP 域复位期间把管脚
   状态锁存住，因此软件可以把 PWR_HOLD 的高电平「带过」复位窗口。
   用 RTC_NOINIT 标记判断复位期间是否真的掉过电。 */
void bsp_power_prepare_download_reset(void);
bool bsp_power_download_survived(void);   /* 上一次复位是否未掉电 */
bool bsp_power_pad_hold_active(void);     /* 本次启动是否检测到残留 hold */
