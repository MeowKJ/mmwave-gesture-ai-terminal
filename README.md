# 毫米波手势 AI 终端

`mmwave-gesture-ai-terminal` — 基于 **鱼鹰光电 ESP32-P4C5（OspreyPi-P4C5）** 的毫米波手势交互终端固件，ESP-IDF v6.1。

一根 USB-C 同时完成 **无感烧写、CDC 日志、HID 输入**；屏上跑 LVGL 9.4 + CO5300 AMOLED 触摸交互，毫米波雷达走 UART 接入。

## 硬件

| 模块 | 器件 / 接口 |
| --- | --- |
| 主控 | ESP32-P4C5 模组（ESP-IDF v6.1, `IDF_TARGET=esp32p4`） |
| 显示 | CO5300 AMOLED 460×460，MIPI-DSI 1-lane，AM200MT460460FLS2 |
| 触摸 | CST820，I²C（GPIO8/9/10/11） |
| 音频 | ES8389 codec，I²C + I²S0 48 kHz 双向 |
| 雷达 | 毫米波模块，UART + 电源/复位控制 |
| USB | 原生 USB 2.0 HS OTG，TinyUSB CDC-ACM + HID 复合设备 |
| 电源 | `PWR_HOLD`(GPIO29) 自锁 / `VBUS_ON`(GPIO28) 输入检测 / 电池 ADC |

完整 GPIO 映射见 [`firmware/p4_gesture/main/board_pins.h`](firmware/p4_gesture/main/board_pins.h)。

## 核心特性

### 1. USB 无感下载（无需按 BOOT / 无需人工操作）

```
主机 flash_usb.py              设备运行态 CDC 303A:4005
   │  magic "MEOAVE-DOWNLOAD\n" / DTR-RTS 脉冲
   ▼
bsp_usb.c：关闭大电流负载 → tud_disconnect()
        → gpio_hold_en(GPIO29) 跨复位锁住 PWR_HOLD
        → LP_SYS FORCE_DOWNLOAD_BOOT (0x50110008 bit2)
        → esp_restart()
   ▼
ROM USB-OTG CDC 303A:0012
   ▼
esptool --before no-reset --after hard-reset（USB-OTG 走 watchdog_reset）
   ▼
烧写完成，自动回到应用态 CDC 303A:4005
```

应用态 PID `0x4005` = `0x4000 | CDC(1) | HID(4)`；ROM 下载态 PID `0x0012`。
触发路径共四条：主机 magic、DTR/RTS 兼容脉冲、屏幕 `USB FLASH` 双击、长按电源键（CDC 亦可发 `DOWNLOAD`）。

> 空片首次下载仍需按住 BOOT 上电烧一次，之后全程无感。

### 2. 显示与触摸

- 官方 `esp_lcd_co5300` + 官方初始化序列 + `esp_lvgl_port`，RGB565 / DPI。
- 开机 5 色纯色自检（验证 DPI 真的输出），随后交给 LVGL；状态栏实时显示 **VSYNC / FPS**，`vsync>0` 才说明 DPI 输出已打开。
- CST820 注册为 LVGL indev，触摸画点、坐标显示，并同步发 HID 报表。
- 没焊触摸芯片也能测渲染链路：CDC 发 `TOUCH 230 230` 注入合成触点。

### 3. DEMO #1

460×460 屏上显示 USB / VSYNC / FPS / 电源 / 触摸 I²C 状态；`RESCAN` / `USB FLASH`（双击确认）/ `CLEAR` 按钮；开机自检与心跳标记供主机脚本判定 PASS/FAIL。

CDC 交互指令：`HELP / STATUS / SCAN / TOUCH x y / COLORS / CLEAR / HID / DOWNLOAD`。

## 目录

```
firmware/p4_gesture/
├── main/                  BSP（display/touch/audio/radar/usb/power）+ 应用 + DEMO UI
├── components/            CST820 触摸驱动（Espressif, Apache-2.0）
├── .vscode/               构建 / 烧写 / 测试任务
├── flash_usb.py           无感下载主机脚本（自动找 4005 → 等 0012 → 烧三镜像）
├── demo_test.py           一键测试：产物校验 + 烧写 + 日志断言
├── sdkconfig.defaults     可复现配置（sdkconfig 为生成物，不入库）
└── partitions.csv
```

`managed_components/`、`build/` 由 ESP-IDF 组件管理器和构建系统生成，不入库；依赖版本锁定在 `dependencies.lock` 与 `main/idf_component.yml`。

## 构建

需要 ESP-IDF **v6.1** 与 RISC-V 工具链：

```bash
cd firmware/p4_gesture
idf.py set-target esp32p4
idf.py build
```

## 烧写与测试

```bash
cd firmware/p4_gesture
python demo_test.py --check      # 不接板：校验产物 + 烧写命令 + esptool
python demo_test.py --run        # 接板一键全流程，末尾给 PASS/FAIL 汇总
python demo_test.py --monitor    # 手动摸屏，看实时日志
python flash_usb.py --dry-run    # 仅打印将要执行的 esptool 命令
```

VSCode 里也可直接跑任务 `DEMO #1 - run`。依赖 `pyserial` 与 `esptool`。

## 当前状态

- ✅ 沙盒外 IDF 原生环境 `ninja -t clean` 后全量 `ninja all`，`exit=0`；自有代码 0 warning。
- ✅ `demo_test.py --check`、`flash_usb.py --dry-run`、反汇编校验（`0x501101c4` pad hold、`0x50110008` force-download）均通过。
- ⚠️ **新板尚未接入实测**：烧写、点屏、物理触摸、ROM PID 重枚举、`hard-reset` 回应用均待上板验证。
- ⚠️ 硬件建议：原理图中 `R13(470K)` 把 `PWR_HOLD` 下拉到 GND，复位窗口 GPIO 高阻可能导致 3V3 掉电。固件已用 LP_SYS 常开域 pad hold 跨复位锁存，量产仍建议把 R13 改接 `VBUS_ON` 作为硬件保险。

## 许可

本仓库尚未指定开源许可证。`components/esp_lcd_touch_cst820` 为 Espressif Systems 的 Apache-2.0 组件（保留原版权头）；CO5300 初始化序列参考 Osptek 官方 AM200MT460460FLS2 例程；其余依赖由 ESP-IDF Component Manager 按各自许可证拉取。
