# 自制板 C5 原生 USB 维护

本入口与开发板 UART 专用维护固件分开；不能把无屏开发板 P4 固件烧到自制板。

## 链路和边界

电脑 → P4 原生 USB CDC（303A:4005）→ 专用维护任务 → ESP-Hosted/SDIO → C5。

- `maintenance` 产品 profile 保留原显示/触摸初始化、GPIO29 保持、USB 安全下载/复位、双核 UI、单次 180° 旋转。
- 直接编译复用 `bringup/main/main_maintenance.c` 的 RPC/OTA 状态机，不另写一套升级算法。
- USB 收包回调只验证/排队，阻塞 SDIO/RPC 在独立任务运行。单条事务最多一个在途，不能在 TinyUSB 回调里等 RPC。
- 每条 `C5M <随机ID> <命令>` 都有匹配的 BEGIN/END 边界；END 只是完整传输标记，不等于命令或升级成功。
- 接收上限 1400 字节，支持 512B 十六进制 OTA 数据；超长输入整行丢弃，绝不把尾部解释为下载/复位命令。
- ATTACH 不复位 C5；CONNECT 才请求一次共享 GPIO54/EN 复位。仅适用于当前音频/雷达未启用的产品 profile。
- GPIO6/PIN 与 NET 命令暂拒绝；产品音频仲裁、Wi-Fi UI/凭据和业务联网另验，不继承开发板验收。
- P4 重启只走现有 `SYS RESTART`，不调用无屏程序的 `P4RESET`。
- C5 尚未安装维护服务时，无法用该服务给自己首次安装；必须保留首次下载/备份路径。

## 命令（仓库根目录）

```powershell
python tools/meoave.py build --profile maintenance

# --build-dir 使用绝对路径，或相对 firmware/p4_gesture 的目录名。
# 只更新 P4，不写 C5。
python firmware/p4_gesture/flash_usb.py --build-dir build_meoave_maintenance --report firmware/p4_gesture/test-results/native_p4_flash.json

# 默认不复位 C5。报告使用新文件名，禁止覆盖旧证据。
python tools/c5_native.py check --port COM10 --report firmware/p4_gesture/test-results/native_c5_check.json

# 仅在明确允许一次 C5 EN 复位、音频/雷达停用时使用。
python tools/c5_native.py check --port COM10 --reset-c5 --report firmware/p4_gesture/test-results/native_c5_connect.json

# 仅适用于已经装好维护基线的 C5；不是首次烧录方法。
python tools/c5_native.py update --port COM10 --report firmware/p4_gesture/test-results/native_c5_update.json
```

构建目录独立为 `build_meoave_maintenance`，机器可在忽略的 `tools/config.toml` 设置 `[build] maintenance_dir`。
不修改原 product/hosted/programmer 缓存；首次需要编译新依赖，后续增量编译。
USB 工具以 VID/PID、完整 P4 ELF、随机事务 ID 和 C5 实时 RPC 为判据，不把端口枚举或缓存 IDLE 当作 C5 正常运行。

## 实施进度（2026-11-11）

- 原板 P4 为 fa8de08d… 下载诊断镜像，已找到同 SHA 本地镜像并留存恢复副本。
- 原板非复位 SDIO 采样 20/20 成功、读取区域全零，无应用签名；不能判断 C5 损坏或运行模式。
- 新 native profile 编译成功，197 项离线回归通过，包含实际 C USB 解析器的长行/分包/尾部注入测试。
- 新 P4 镜像与旧板的分区表逐字节一致；UI/电源/USB/PSRAM/色深/双核关键配置对比一致。
- 新 P4 ELF `b5278b7796a8cb3d7b630d58da76e2443bfa8a36cb897e7e3a7463a166d0e1b7` 已经原生 USB 写入并校验通过，
  自动 CPU-only 启动、完整 SHA/新 boot/GPIO29 保持通过（`custom_p4_native_maintenance_flash_20261111_r2.json`）。
  首次命令因相对路径基准错误，在 Flash 操作前失败，独立报告保留；没有用失败结果冒充烧录成功。
- 默认不复位 ATTACH：SDIO 卡初始化成功、寄存器可读，但 10000ms 内无 Hosted INIT，返回 0x107；
  `custom_c5_native_attach_20261111.json` 为 false，P4 同 boot/USB/供电/FAULT valid=0 保持。
- 随后只做一次 P4 安全程序复位（`custom_p4_native_maintenance_restart_20261111.json` 为 true），
  明确 CONNECT 请求一次 C5 EN；卡初始化仍成功，仍无 Hosted INIT，返回 0x107。
  `custom_c5_native_connect_20261111.json` 为 false；不是循环复位/重试，不宣称 C5 损坏或确定处于下载模式。
- `custom_p4_native_runtime_20261111.json` 自动运行/触摸/USB/电源检查通过，无当前有效 fault；
  用户随后独立确认更新/程序复位后“显示正常”。该确认属于 native 镜像，不代替后续镜像的目视验收。
- 用户未确认下载三脚接地，明确要求“不复位 C5，直接检测能否 SDIO 下载”。因此未操作下载脚、GPIO6，未写 C5 Flash。
- 仅将 P4 切回已留存的产品下载诊断镜像 fa8de08d…，未重新编译；原生 USB 三段校验和 CPU-only 启动通过，
  见 `custom_p4_no_c5_reset_programmer_20261111.json`。没有请求 C5 EN 脉冲；没有示波器测量，
  不能证明 P4 重启期间 C5 EN 无瞬态或 C5 uptime 连续。
- 2026-11-11 21:00（UTC+8），新 IDLE 状态下仅发送一次 `C5 CONNECT`（不是 BOOTCONNECT），
  返回 `FAILED stage=CONNECT error=6 reset=never`；本地官方 loader 枚举为 `ESP_LOADER_ERROR_INVALID_TARGET`，
  不得误写成超时、芯片损坏或 GPIO 电平测量。未得到可用芯片/Flash 身份，未执行后续 Flash 读取或写入。
  报告 `custom_c5_no_reset_download_probe_20261111.json` 为 false，原始失败保留，无复位/自动重试。
- 本次连接前后 P4 boot `4ff68843` 不变、uptime 增加，USB/GPIO29 保持正常，FAULT valid=0。
  当前 P4 留在 fa8de08d… 下载诊断 profile，不是 b5278b77… Hosted native profile；C5 首装和 native OTA 尚未验收。
  当前状态下不能直接下载，但无法只凭此结果确定 C5 当前应用或 ROM 启动模式；后续进入下载模式/复位须先另获用户确认。
- 后续用户报告三脚手动拉低；新一轮无复位 CONNECT 仍返回 INVALID_TARGET(6)。该报告不是 GPIO 电压测量。
- 21:07（UTC+8）用户明确允许一次 C5 复位后，先用 P4-only 程序复位清除诊断 FAILED，
  仅发送一次 BOOTCONNECT。返回 `FAILED stage=RESET_ONCE error=1`，没有进入本次下载握手。
  `custom_c5_single_reset_download_20261111_210748.json` 保留原始证据：P4 pad 读回
  before=1/low_begin=1/low_end=0/high_begin=1/high_end=1，低阶段约 9940us，
  peripheral=1/sleep=1，检查未通过。不能据此宣称 C5 端有效 EN 波形或复位完成，也不直接断言硬件故障。
- 没有再次发送 C5 复位；只再次 P4-only 程序复位清除失败锁存，随后一次无复位 CONNECT 仍返回 INVALID_TARGET(6)，
  见 `custom_c5_after_single_reset_connect_20261111_210833.json`。连接前后 P4 boot f7974b91 不变、
  USB/电源/FAULT valid=0 正常。全程无 C5 Flash 写入；下一步调查 GPIO54 复位控制与 SDIO 身份识别，禁止用循环复位掩盖失败。

## 2026-11-11 21:33 手工 strap 抓窗口 → 首次完整写入通过

- 新增 programmer 固件命令 `C5 WATCH <token> <次数>`：每 1s 一次 GPIO54 EN 脉冲（20ms）后立刻做一次 SDIO 下载探测，
  命中即停、保留 loader 会话并读到 FACTS，随后才能接 `C5 FLASH`；最多 240 次，仅在 loader IDLE 时可启动或重新武装。
  驱动见 `tools/c5_watch.py`（`--attempts/--period/--start-delay/--flash`）。
- `BOOTCONNECT` 之前卡在 `RESET_ONCE` 的根因已确认：旧 `reset_c5_once` 要求拉低后**第 1 个 1ms 采样必须为 0**，
  实测该点为 1（10ms 后才为 0），于是自判失败、握手根本没有执行。WATCH 改为统计 20 个 1ms 采样中的低电平个数，不再用首采样一票否决。
- 结果 `c5_watch_20261111_r1.json`：第 1 次窗口即命中（`strap=0x22`，GPIO26/27/28 读回 level=0，20/20 低电平采样），
  `chip=6`、`flash_size=4194304`；写入 1352112/1352112 字节，`VERIFY_MD5` 通过，终态 `state=DONE stage=VERIFIED`。
- 本次写入四区域：`0x2000` bootloader、`0xc000` 分区表、`0x16000` otadata、`0x20000` `network_adapter.bin`（1316640B，ELF b97127f1…），
  载荷 SHA-256 前缀 `49aeca8c…`。P4 boot `bb5592f6` 前后不变、uptime 增长，FAULT valid=0。
- 未验证：C5 应用是否启动、Hosted INIT、维护服务与联网。写完后 C5 仍停在 ROM 下载模式，须释放 strap 后复位一次才会运行新固件。

首次 C5 安装应先取得本板完整 4MB 备份和当前芯片/启动脚证据，再校验布局、版本、目标写入范围。
不可拿另一颗 C5 的备份恢复本板，不整片擦除，不覆盖 NVS/标定/分区表，不把应用 bin 写到地址 0。

## 2026-11-11 21:35 免按键维护首次实测：ATTACH 通过，OTA 在 55% 被 P4 看门狗打断

- 完整固件写入后释放 strap，仅一次共享 EN 脉冲（`c5_native.py check --reset-c5`）即得到 Hosted ATTACH：
  C5 `boot=8ed2c7aa`、`slot=0x20000`、`ota_state=2`、`elf_sha=b97127f1…`，P4 `b5278b77…`、FAULT valid=0。
  这是自制板第一次在没有按键、没有碰 strap 的情况下进到维护通道。
- `c5_native.py update`（免按键 SDIO A/B）在 `918016/1316640` 处中断，USB 报 `ClearCommError/PermissionError`，
  设备消失约 8 秒后重新枚举；报告保留为 `native_c5_keyless_update_20261111.json`（ok=false）。
- P4 复位原因 `reset_reason=7`（WDT），不是 BROWNOUT(9)、不是 PANIC(4)；FAULT 记录 valid=0（字段为未初始化值）。
  时间线旁证：P4 `boot 85076023 → b4925ad5`，C5 `boot 8ed2c7aa → d856c393`，两者 uptime 只差约 0.3 秒，
  说明 **P4 复位期间 GPIO54 会把 C5 一起按进复位**（此前"P4 重启不影响 C5"未获证，这次有了反证）。
- A/B 保护生效：中断后 C5 仍从 `ota_0`（`slot=0x20000`、`ota_state=2`）启动同一份 `b97127f1…`，
  旧槽完好、无变砖；另一个槽是未写完的半成品，未被激活。
- 现场不可见的原因：维护 profile 的 console 是 **UART0**（`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`），
  不是原生 USB CDC，所以 WDT/panic 现场不会经 COM10 出来。下一步要么接 UART0 抓现场，要么把写入阶段的关键事件改走 `MEO:` 行。

## 2026-11-11 22:20-23:05 产品固件：电量显示、长按关机、Wi-Fi 实装

- 电量：`bsp_power_battery_percent()` 五次采样取均值 + 单体锂电分段曲线；`bsp_power_charging()` 取 VBUS 分压。
  首页右上角显示 Wi-Fi 图标、百分比与电池条，充电时用青色（字体里没有闪电符号，用颜色表达）。
  实测 `MEO:UI ... battery=93 charging=1`，ADC 与分压链路可用。
- 长按关机：`bsp_power_off()` 先 `gpio_hold_dis(GPIO29)` 再拉低，关机钩子用 `s_poweroff` 阻止复位时重新锁存。
  按键状态机在 `meoave_app_key_tick()`：<2s 短按回主菜单；持续 2s 触发 5s 倒计时（顶层遮罩、大字数字、可再按取消）；归零后拉低 PWR_HOLD。
  USB 供电时 3V3 不会掉，此时熄灭屏幕并保持 PWR_HOLD=0，USB 仍可恢复。
- Wi-Fi：`bsp_wifi_hosted.c` 重写为完整 STA：后台 bring-up、`WIFI_BAND_MODE_AUTO`、扫描/连接/NVS 凭据、事件驱动状态机；
  `BSP_WIFI_*` 状态每 1s 喂给 UI；USB 命令 `WIFI STATUS|SCAN|APS|JOIN|SET`。
  实测 Hosted 传输在 40MHz 4-bit 下跑通，扫描 `raw=16`（C5 侧 RPC 收到 16 个 AP）。
- 修掉两个真实崩溃（都是栈溢出，不是 SDIO 硬件问题）：
  1) `esp_event` 任务栈只有 2304B，在 SCAN_DONE 回调里取 6 条 `wifi_ap_record_t` + RPC 必然溢出；
     改为回调只置 `s_scan_ready`，取结果搬到 app 侧。证据：`mcause=5` load fault @ `esp_event_loop_run`。
  2) 搬到 `app_main` 后仍然溢出（`badaddr=0xfffffbbf`，pc 在 `app_main` 循环），改为独立 `wifi_service` 任务（8KB）。
- 待办：AP 记录解码（第 1 条 SSID 正常、后续为空/rssi=0，怀疑隐藏网络或 host/slave 结构体差异）、实际 join、屏幕选网 UI。
