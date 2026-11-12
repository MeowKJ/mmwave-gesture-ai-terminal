# Meoave P4

Meoave 是基于 OSPTEK ESP32-P4C5 自制板的桌面陪伴设备。当前仓库只保留一个固件工程：`firmware/p4_gesture`；统一编译、测试和原生 USB 下载入口在 `tools/meoave.py`。

屏幕为 460×460 **方形** AMOLED，显示方向旋转 180°。UI 与预览均使用完整方形画布，圆角仅用于卡片，不用于裁切屏幕。

## 快速开始

```powershell
Copy-Item tools/config.example.toml tools/config.toml
python tools/meoave.py doctor
python tools/meoave.py all
```

机器相关的 ESP-IDF、CMake、Ninja 和 Python 路径只写入本地 `tools/config.toml`，不会进入 Git。

## 目录

- `firmware/p4_gesture/`：唯一 P4 固件工程、UI、板级驱动和测试
- `firmware/p4_gesture/main/fonts/`：LVGL 中文点阵字库与 `manifest.json` 生成清单
- `tools/`：稳定的构建、测试、硬件验收入口
- `docs/datasheets/`：用到的芯片与模组数据手册（PDF 不入库，索引见 `docs/datasheets/README.md`）
- `PROJECT_PLAN.md`：当前目标、边界和验收标准

界面字体是资源圆体 Resource Han Rounded SC（SIL OFL 1.1）。字体源文件（约 120 MB，六套可选字体）不打进仓库，放在 [fonts-v1 Release](https://github.com/MeowKJ/mmwave-gesture-ai-terminal/releases/tag/fonts-v1)，`python tools/meoave.py fonts --fetch` 会下载并逐个校验 SHA-256。

改了文案要增删汉字时，先补 `main/fonts/manifest.json` 的符号集，再跑 `python tools/meoave.py fonts` 重生成；`--set round|round_bold|chill|wenkai|yozai|maiyuan` 可整套换字体。

## 重要边界

- 下载只走主板原生 USB；不使用 COM3/WCH-Link。
- GPIO6 保留给音频 I2S DOUT，不参与 Wi-Fi SDIO。
- P4C5 Wi-Fi 使用 GPIO14–19 的 4-bit SDIO；GPIO54 与雷达 SOP0 复用，必须受安全时序约束。
- Hosted profile 目前只是独立编译接入，未证明 C5 已烧录、已握手或已联网。
- 首次冷启动仍需要板上开机动作；已开机后的下载和程序复位目标是不再按键。

## 产品功能（电量 / 长按关机 / Wi-Fi）

细节见 `docs/PRODUCT_UI_POWER_WIFI.md`。要点：

- 首页右上角显示 Wi-Fi 图标、电量百分比与电池条；插 USB 时转青色。
- 短按电源键回主菜单；按住 2 秒进入 5 秒关机倒计时，再按一次取消。
- Wi-Fi 走 ESP-Hosted（C5），凭据存 NVS、开机自动重连，屏幕「设置 → 网络」可扫描并点选已保存的网络；
  也可用 USB 命令 `WIFI STATUS|SCAN|APS|SET|JOIN`。Wi-Fi 产品镜像使用 `--profile hosted`。
