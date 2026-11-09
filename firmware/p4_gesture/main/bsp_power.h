#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* ---------------------------------------------------------------------------
 * 电源键 = 开机键（本版本只开机，不做任何软件关机）
 *
 * 依据 Schematic4 POWER 页 + Netlist_Schematic4_2026-11-06.tel 核对：
 *   SW1(TS6611B3JTP, 1/3 接 GND) 按住 -> $2N45 接地 -> D1(BAT54W) 把
 *   Q1(AO3401A, P-MOS) 栅极网络 $2N41 拉低 -> VSYS 接通 SYS_PWR_EN(U10 EN) ->
 *   U10(SY8089) 出 3V3 -> P4 启动；同时 D2 把 PWR_DET(GPIO30) 拉低。
 *   固件起来把 PWR_HOLD(GPIO29) 拉高 -> Q2(AO3400A, N-MOS) 导通接管
 *   $2N41 -> 松开 SW1 后 Q1 仍导通，即「按了以后开机」。
 *
 * 硬件约束（R10 470K 上拉 $2N41、R13 470K 下拉 PWR_HOLD）：
 *   1. 必须按住 SW1 直到固件跑到 bsp_power_init()（ROM + bootloader 数百 ms），
 *      不能点一下就松手：GPIO29 在固件配置前是高阻，松手即掉电。
 *   2. 插 USB 不会自动开机，VBUS 只到 U4 充电芯片与 VBUS_ON 分压检测。
 *   3. 软件复位窗口 GPIO29 变高阻 -> Q2 关断 -> 掉电；这是下载模式要靠
 *      LP_SYS pad hold 跨复位锁存的原因。
 *
 * 软件关机在硬件上可行（拉低 PWR_HOLD 即可断电），但当前版本刻意不实现：
 * 不提供任何拉低 PWR_HOLD / 断电的接口，也没有 deep-sleep poweroff。
 * ------------------------------------------------------------------------- */

/* Call first at boot: preload high, enable output and keep GPIO29 pad hold
   continuously enabled. A shutdown hook covers normal esp_restart() calls. */
void bsp_power_init(void);
esp_err_t bsp_power_prepare_reset(void);
bool bsp_power_hold_ready(void); /* Output latch, output enable and pad hold. */
void bsp_power_reset_diagnostic(char *buffer, unsigned size);

int  bsp_power_battery_mv(void);       /* 失败返回 -1 */
bool bsp_power_usb_attached(void);      /* VBUS 分压检测 */
bool bsp_power_key_pressed(void);       /* 电源键当前是否按下（低有效） */
bool bsp_power_key_at_boot(void);       /* 开机锁存瞬间 SW1 是否仍被按住 */
bool bsp_power_buck_ok(void);

/* ---------- 无感下载专用的电源保持 ----------
   HOLD 电路：PWR_HOLD(GPIO29) -> Q2 -> Q1 -> 3V3；R13 470K 把 Q2 栅极
   下拉到 GND。芯片复位时 GPIO 变高阻，Q2 关断，3V3 会掉电，烧写随之中断。
   P4 rev >= 3.0 的 pad hold 寄存器位于 LP_SYS 常开域
   (LP_SYSTEM_REG_PAD_RTC_HOLD_CTRL0_REG)，能在 TOP 域复位期间把管脚
   状态锁存住，因此软件可以把 PWR_HOLD 的高电平「带过」复位窗口。
   RTC_NOINIT 仅作为保留标记，不是电压/断电测量。 */
void bsp_power_prepare_download_reset(void);
bool bsp_power_download_survived(void);   /* Previous reset marker retained. */
bool bsp_power_pad_hold_active(void);     /* 本次启动是否检测到残留 hold */
