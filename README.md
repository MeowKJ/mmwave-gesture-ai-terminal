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
- `firmware/p4_gesture/main/assets/`：已锁定来源/许可的 Lucide SVG、CC0 试听音乐及生成清单
- `tools/`：稳定的构建、测试、硬件验收入口
- `docs/datasheets/`：用到的芯片与模组数据手册（PDF 不入库，索引见 `docs/datasheets/README.md`）
- `PROJECT_PLAN.md`：当前目标、边界和验收标准

当前界面字体为寒蝉圆黑体 ChillRoundGothic Medium（SIL OFL 1.1），以字库 `manifest.json` 为准。字体源文件（约 120 MB，六套可选字体）不打进仓库，放在 [fonts-v1 Release](https://github.com/MeowKJ/mmwave-gesture-ai-terminal/releases/tag/fonts-v1)，`python tools/meoave.py fonts --fetch` 会下载并逐个校验 SHA-256。

改了文案要增删汉字时，先补 `main/fonts/manifest.json` 的符号集，再跑 `python tools/meoave.py fonts` 重生成；`--set round|round_bold|chill|wenkai|yozai|maiyuan` 可整套换字体。

## 重要边界

- 下载只走主板原生 USB；不使用 COM3/WCH-Link。
- GPIO6 保留给音频 I2S DOUT，不参与 Wi-Fi SDIO。
- P4C5 Wi-Fi 使用 GPIO14–19 的 4-bit SDIO；GPIO54 与雷达 SOP0 复用，必须受安全时序约束。
- 产品构建现默认包含 C5 Hosted Wi-Fi，并检查最终 ELF 的 Remote ABI；当前实机验收状态见 `docs/WIFI_AUDIT_20261113.md`，编译成功不代表已联网。
- 首次冷启动仍需要板上开机动作；已开机后的下载和程序复位目标是不再按键。

## 产品功能（电量 / 长按关机 / Wi-Fi）

产品行为见 `docs/UI_INTERACTION_20261112.md`，Wi-Fi 验收见 `docs/WIFI_AUDIT_20261113.md`。要点：

- AI 页只有黑白眼睛；Wi-Fi、电量百分比与电池条只在 App 主页显示，不把 USB 供电误称为充电。
- 短按电源键回 AI；持续按住 1.5 秒后进入 **3 秒**关机倒计时，松手取消（总持续按住约 4.5 秒）。
- 麦克风页自动开始电平检测，离页停止；设置行只显示真实设置百分比，不重复“灵敏度”文字。扬声器试听为约 1.32 秒 CC0 拨弦音乐，遵循当前音量，包括静音；“咚”交互音在辅助功能开关，最新听感验收状态见 `docs/UI_POLISH_20261113.md`。
- Wi-Fi 走 ESP-Hosted（C5），凭据存 NVS、开机自动重连，屏幕「设置 → 网络」可扫描并点选已保存的网络；
  也可用 USB 命令 `WIFI STATUS|SCAN|APS|SET|JOIN`。默认 `--profile product` 已包含 Hosted Wi-Fi，不必另换 profile。
- AI 对话当前已提供离线垂直切片，可用 USB 命令 `AI DEMO|STATUS|CANCEL` 检查状态与眼睛表情联动；
  该演示不读麦克风、不联网、不保存录音，真实云端对话仍按 `docs/AI_VOICE_PLAN.md` 分阶段接入。

最新小屏网络布局、三列输入法、Lucide 图标与音乐来源见 `docs/UI_POLISH_20261113.md`；其他 UI 行为及历史实机验收见 `docs/UI_INTERACTION_20261112.md`。
