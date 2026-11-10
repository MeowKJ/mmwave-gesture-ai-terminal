# C5 首次烧录、后续升级与 GPIO6 分时

日期：2026-11-09。硬件：鱼鹰光电 P4C5 自制底板 V1.0。

**2026-11-10 16:14 最新：三脚互联已解除，原生USB COM10恢复。** 一次C5复位后仍无
有效诊断签名/心跳，未进入回显；随后只重启P4、单次CONNECT返回INVALID_TARGET=6，
未取得新启动脚读数，不能继续说三脚当前000或当前stub在运行。当前需补CIS身份/初始化
证据；不认定芯片损坏，不重复C5复位/盲刷。本轮无Flash写入/编译/GPIO6操作；详见计划第17节。

**2026-11-10 05:05 全局复查优先：** P4已换boot_id=b68a43de，上次复位原因7
（ESP_RST_WDT），原因未明，不能全局判通过；当前USB/供电/UI/触摸读取通过。
C5加载器初始IDLE、FACTS无效；20次被动CMD52读取成功但全零，无有效应用心跳。
本轮未CONNECT/上传RAM助手，不能沿用“当前stub仍在运行”或先前三脚低的结论；
当前C5模式/三脚实时电平未知。未复位、烧录或操作GPIO6；详见计划第16节。

**2026-11-10 04:34 最新进度：** 用户接入3.3V后的一次启动验证仍无有效签名/心跳。
随后只重启P4释放owner，新IDLE→CONNECT成功，04:31新采样仍读C5三脚000，非旧缓存。
同一stub会话只读回281760 B启动区+诊断应用，与备份+诊断逐字节一致且镜像校验通过。
当前需人工核对模组脚号/3.3V取电点/公共GND/连接的照片；无需拆分互联或COM3。
不把数字0当作测得0V，不判芯片损坏，不继续盲刷。未再次复位/写Flash C5或操作GPIO6。
当前仍为最小诊断Flash+官方RAM stub，Hosted/Wi-Fi/OTA未验收。详见计划第15节。

**2026-11-10 最新状态优先：** 解除三脚后的最小应用仍未执行；新取证读到 C5
GPIO26/27/28 当前全部为低，GPIO27/28 输入及内部上拉均开启、GPIO 输出未开。
锁存 GPIO28=0，未复位 CONNECT 下载成功。先查启动脚电气释放（特别模组6脚），
不要继续将它当成 Hosted/Wi-Fi 握手故障。用户已确认拆线；低电平具体来源未知，
不宣告芯片损坏，不强接3.3V、不驱动GPIO6。本轮未写C5 Flash；当前 Flash 是最小
诊断应用、RAM 是官方下载 stub，原 Hosted 未恢复。完整证据见 PROJECT_PLAN 第13节。

## 首次烧录

已实测：P4 原生 USB COM10 可用，C5 在用户手动切换启动模式后，首次返回
有效 CMD5 R4（OCR=0x2100ff00，2 functions）。R4 只证明 SDIO 有应答；芯片识别、
Flash 写入/校验、Hosted 启动分别验收。

```powershell
python tools/meoave.py c5
python tools/meoave.py build --profile programmer
python tools/meoave.py c5-program --install          # 更新 P4，识别 C5，不写 C5 Flash
python tools/meoave.py c5-program --flash            # 写入并逐段 MD5 校验
```

`programmer` 是独立配置和构建目录，保留产品 UI、原生 USB 和 GPIO29 电源保持。
使用官方 esp-serial-flasher 2.0.0 的 SDIO 协议，配自定义手动进 boot 的端口：

- SDIO：P4 D0–D3=14–17，CLK=18，CMD=19，最高 5 MHz。
- `enter_bootloader/reset_target` 均为空操作；不配置/驱动 GPIO6、GPIO54 或 strap。
- 不与 handle-based SDIO 探针、引脚测量或 Hosted 运行态并发使用控制器。
- CONNECT 必须识别 ESP32-C5、Flash 至少 4 MB，并核对 P4 内嵌镜像 SHA 后才能 FLASH。
- 按 C5 `flasher_args.json` 的实际偏移与长度烧写四个镜像，保留 NVS、PHY、工厂数据；不整片擦除。
- CONNECT/FLASH 指令均只发送一次。失败记录阶段与错误，不自动拉复位或重刷。
- MD5 全部通过只表示写入成功。首次启动需要释放人为接地的 strap，再重新上电。

## 后续免引脚升级

首次从机固件带 ESP-Hosted OTA RPC 和两个 OTA 应用分区。正常运行后的升级链路：

1. 电脑经 P4 原生 USB 交付 **C5 应用镜像**（`network_adapter.bin`，不是合并镜像）。
2. P4 通过已有 SDIO Hosted RPC 调用 `esp_hosted_slave_ota_begin/write/end`。
3. C5 写入非运行 OTA 槽并校验应用镜像；成功后才调用 `...ota_activate` 切换并软件重启。
4. P4 重新握手，核对从机版本与启动后的身份；不触碰 GPIO54、GPIO6 或 BOOT strap。

官方实现：本仓库 `components/esp_hosted/host/api/include/esp_hosted_ota.h`、
C5 `main/slave_control.c`；分区表为 `firmware/c5_hosted/partitions.esp32c5.csv`。

状态：C5 端 OTA RPC 已有；P4 产品工具的 OTA 传输与实机完整升级验收必须另行完成，
不能把首次 ROM 烧录成功当作“免引脚 OTA 已通过”。C5 应用无法运行时，仍需 ROM 恢复入口；
当前板上 P4 没有连到 C5 的 GPIO26/27/28。

## GPIO6 / C5 GPIO2 分时设计

两端与 ES8389 的 SDI 在同一根电线上：P4 GPIO6 ↔ C5 GPIO2。
SDIO Hosted 可通过 GPIO RPC 配置 C5 的 GPIO2，这是**远程软件控制 pad**，不会改变物理接线。
Wi-Fi 控制/数据持续使用 SDIO，无需占用 GPIO6。

| 状态 | P4 GPIO6 | C5 GPIO2 | 音频链路 |
|---|---|---|---|
| 默认/启动/异常 | 不主动增加驱动 | 输入、无上下拉、高阻 | 未准备好前不播放 |
| 音频工作 | I2S TX 输出 | 高阻 | 正常播放，可同时使用 SDIO Wi-Fi |
| C5 临时使用 | I2S TX 禁用并解除 GPIO 输出路由，输入态 | 经授权的 GPIO 模式 | 静音、停 DAC 数据/相关时钟 |
| 恢复音频前 | 保持输入 | 先切高阻，等远端 ACK | ACK 成功后恢复 I2S 路由和音频 |

所有权切换必须有单一管理者、序号、超时和明确 ACK；“停 DMA”本身不能证明 P4 pad 已高阻。
远端失联或 ACK 不确定时，P4 不能恢复推挽 I2S 输出。C5 临时输出应有租约，到期/断链自动回高阻。
GPIO2 与 C5 BOOT strap 无物理连接，不能靠分时 GPIO6 替代启动 strap。

`c5_io2_guard` 保留；GPIO 扩展 RPC 现已在 C5 从机与 P4 主机两侧开启
（`CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER=y`），P4 可经 SDIO 配置/读写 C5 GPIO2。
注意它的能力边界：只覆盖 GPIO 输入/输出/开漏、电平、上下拉与回读，**不能把 GPIO2
改配成 C5 的某个外设功能**（I2S/UART 等仍须 C5 侧固件自己申请 pad）。分时所有权协议、
音频静音/路由切换、失联回高阻及实际电气时序尚未实现/验收；不能因为打开了通用 GPIO RPC
就宣称安全分时。

## 2026-11-09 晚：实机烧录与联调记录

### 写入结果（已验收）

- 通过 P4 原生 USB 进入 ROM，写入 `programmer` 固件；bootloader/分区表/应用三段
  `Hash of data verified`（应用 2 443 792 B）。此前 180 s 超时是主机侧 `esptool`
  子进程超时，写入本身完整：事后 `verify-flash` 对 bootloader 与分区表校验通过。
- `c5-program --flash` 写入 C5 四个区域，共 1 341 488 B：
  `0x2000` bootloader 24 048、`0xc000` 分区表 3 072、`0x16000` otadata 8 192、
  `0x20000` 应用 1 306 176（`payload=d6f7228e…d1b9`）。
- 烧录器回报 `state=DONE stage=VERIFIED verified=1 written=total`，`chip=6`、
  `flash_size=4194304`；分区表 `ota_0/ota_1` 各 1.875 MB，满足后续 OTA。

### 修掉的两个主机侧缺陷

1. `tools/c5_program.py` 的状态正则只允许 `[A-Z_]`，而阶段名 `VERIFY_MD5` 含数字，
   导致“已写完并校验”的会话被判成 `Malformed record` 中止。改为 `[A-Z0-9_]+`，
   并补 `test_stage_names_containing_digits_are_valid`；已 `DONE` 的会话按只读复用，
   不再重发 FLASH。`meoave.py` 的构建签名改为对 `sdkconfig.defaults*` 内容取哈希，
   否则新增 `CONFIG_*` 永远不会进入 `sdkconfig.c5/hosted`。
2. `components/esp_hosted/.../sdio_drv.c::transport_gpio_reset()` 上游以
   `H_RESET_VAL_ACTIVE` 结尾。`CONFIG_ESP_HOSTED_RESET_GPIO_ACTIVE_LOW=y` 时它是低电平，
   等于把 C5 CHIP_PU 一直按在复位里，随后的 `transport_card_init()` 必然失败，失败后
   C5 也一直是死的（实测 `WIFI PINS` 报 `pin54=0`）。本仓库改为“拉低 10 ms → 释放并
  等 `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS`（当前本板设 5000）”，再尝试连接。

### 前次卡点（当时尚未完成冷启动后的独立验证）

释放 CHIP_PU 后 C5 只在最初约 1 s 内对 CMD5 有应答，随后 CMD3 / CCCR 读回
`0x109`、CMD5 退回 `0x107`；重复测试表现为“偶发应答 + 随后消失”。这只能说明
SDIO 初始化未能稳定完成，不能据此证明 C5 再次复位、panic 或供电跌落；这些都只是
待验证的假设，还需排除主机引脚复用、启动模式与总线时序问题。

### 2026-11-09 22:20：本次独立检查

- 起点 P4 reset reason=7，COM10 应用为 Hosted profile，状态 IDLE；本轮没有重刷
  P4 或 C5。此前的 CMD5 探针返回 HOST_BUSY，是 Hosted 已占用 SDMMC 控制器，
  不是 C5 无应答。
- 查出旧 `WIFI PINS` 并非真正的被动测量：它调用 `gpio_config/gpio_reset_pin`，
  把 Hosted 已配置的六根 SDIO 引脚恢复成普通 GPIO，未恢复外设复用。
  Hosted 构造阶段已经分配控制器，即使状态 IDLE 也不能运行该测量或独立探针。
  此前同一启动会话中的 clock-update 超时不作为独立的 C5 故障证据。
- 经已验证的 P4 程序复位恢复总线配置，无需按 SW1；复位后不再发送 PINS/PROBE，
  只发送一次 Hosted CONNECT。驱动内部实际出现三次 C5 reset 尝试，单条 CONNECT
  并不等于内部只复位一次；后续应限制该重试路径，并保留与雷达 SOP0 的共享线保护。
- 本次出现 `Function 0 Blocksize: 512`、`Function 1 Blocksize: 512`、
  `Card init success`，然后等待 Hosted 从机就绪；没有完成上层握手，后续出现
  CRC 错误 `0x109`、超时 `0x107`，最终 state=FAILED、wifi_started=0。
  这证明有 SDIO 应答和一次成功的卡/功能初始化，但不证明 C5 已进入应用而非 ROM，
  也不证明从机固件版本、Wi-Fi 或 OTA 可用，不能据此认定芯片损坏。
- P4 握手前后 boot ID 一致，供电 hold/ready=1，FAULT valid=0；运行态健康检查通过，
  触摸读取计数继续增加且 driver_err=0。实屏画面本次未重新目视验收。
- 报告：`firmware/p4_gesture/test-results/c5_hosted_clean_check_20261109.json`。
  后续优先隔离 40 MHz/4-bit 数据链路与启动模式；若仍失败，需 C5 启动日志等独立
  证据，不能从主机 CRC/超时直接推断从机重启。当时尚未修改/烧录诊断隔离和返回值修复；
  后续已部署的内容见下一节。

### 后续自动化矩阵（已执行）

`python tools/meoave.py c5-test --build --install` 一次构建/安装后测试六组。
诊断配置只在本次启动有效，每组 P4 程序复位恢复独立环境；使用 Hosted 自己的
SDMMC owner，每组仅一次 C5 复位和卡初始化、4 秒 INIT 等待，不在活动 RX 任务运行时
重复 card_init。GPIO54 共享雷达 SOP0，因此先检查 radar=0；不驱动 GPIO6。

- 1-bit/400 kHz、1-bit/5 MHz、4-bit/400 kHz、4-bit/5 MHz、4-bit/20 MHz：
  卡和功能 0/1 块大小初始化通过，无 CRC/SDIO 命令超时记录；均未在 4 秒内收到 Hosted INIT。
- 4-bit/40 MHz：卡初始化出现 CRC 错误 `0x109`，没有进入 Hosted INIT 等待。
- 必须区分两类超时：前五组最终 `error=0x107` 是 Hosted INIT 等待超时，
  不是 SDIO 命令超时。工具分别统计，不从最终状态码反推底层断线。
- 六组 P4 USB/电源保持/触摸读取进度/FAULT 检查通过；每组仅一次 C5 reset，
  完成后已程序复位至默认 IDLE，`diag=0`。实屏本轮未目视验收。
- 已修复并部署旧引脚诊断与独立探针的 Hosted 隔离、连接错误返回值透传、
  型号/固件版本成功后才能 READY 的约束。诊断 CONFIG/CONNECT 仅一次/boot；
  ACK 丢失只轮询相同 operation，不重发变更操作。Python 回归 69 项通过。

当前没有任何一组 Hosted READY，不能开始 OTA/GPIO2 分时验收，也不能归因为 C5 损坏。
当时仍待确认首次下载接地是否解除；2026-11-10 用户已明确确认模组 6/7/8 脚全部断开，
该待办已关闭，但实际启动模式/从机应用运行仍需独立证据。
报告 `firmware/p4_gesture/test-results/c5_automated_20261109.json`。

### 2026-11-10：20 秒中断/轮询对照（已执行）

解除接地后先以 1-bit/400 kHz 复测仍失败，随后只增量更新 P4 诊断固件一次，
同一构建支持可配置 INIT 窗口和接收轮询。未写 C5、未操作 GPIO6，仍通过原生 USB。

| 条件 | 正常 DAT1 中断 | 100 ms 轮询 |
| --- | --- | --- |
| 位宽/频率 | 1-bit / 400 kHz | 1-bit / 400 kHz |
| INIT 名义窗口 | 20 秒 | 20 秒 |
| 卡初始化 / OPEN_DATA_PATH 写 | 成功 / 成功 | 成功 / 成功 |
| CRC / SDIO 命令超时记录 | 0 / 0 | 0 / 0 |
| 成功的只读快照 | 20 次 | 20 次 |
| pending/token/raw/st/len | 全部为 0 | 全部为 0 |
| Hosted INIT / 版本 RPC | 未收到 / 未执行 | 未收到 / 未执行 |

快照来自同一个 SDIO owner 的 CMD52，只读且不消费 FIFO、不清中断。
两组 ien=0x03、ena=0x00800040；OPEN_DATA_PATH 写成功只代表总线事务成功，
不能替代从机软件已收到/处理事件的证据。轮询无改善也不构成 DAT1 硬件正常的独立证明。
本地官方 C5 IDF 寄存器定义与当前采样偏移一致；从机源码应先加载 RX 缓冲，
再通过 OPEN_DATA_PATH 回调触发启动事件，但这些是源码流程，不是实机执行证明。

因此不再把“接地没拆”“等得不够久”当作已确认原因；当前定位止于 **SDIO 能初始化，
Hosted 启动事件缺失**。需要独立的 C5 启动证据，优先取从机 UART 启动日志；
在此之前不盲目重刷、不认定硬件损坏、不宣称免引脚 OTA 或 GPIO2 分时已可用。
两组测试后 P4 均程序复位回默认 IDLE、diag=0，USB/供电/触摸读取/FAULT 检查通过；
实屏本轮未复验。

报告：`firmware/p4_gesture/test-results/c5_irq_20s_20261110.json`、
`c5_poll_20s_20261110.json`；汇总 `c5_irq_poll_comparison_20261110.json`。


### 2026-11-10：重新进入下载模式后的取证（已执行）

- 仅 P4 原生 USB → SDIO，C5 4 MB 整片备份完成，块内/回传 MD5、磁盘 SHA-256、
  前 128 KB 独立重读一致；未写或擦除 C5 Flash。
- 分区表、bootloader、ota_0 checksum/附加 SHA 通过；四区域历史 payload 重建后与
  11 月 9 日成功烧录记录精确一致。当前存储应用为 network_adapter 2.12.13 / IDF v6.1。
  otadata 擦除时预测回退 ota_0，仅为离线判断，不证明应用真实运行。
- 确认 radar=0 后，由 P4 在 fresh IDLE 显式复位 C5 一次并重新连接下载环境。
  只读 FACTS 取得 C5 v1.2，符合存储镜像 v1.0–v1.99 范围；不读写未知寄存器或写 eFuse。
  GPIO6 未配置/驱动，没有新增 UART 或引脚要求，没有循环复位。
- 收尾 P4 同一 boot_id、USB、电源保持、触摸读取和 FAULT valid=0 检查通过，
  实屏未目视复验；P4 保留 programmer profile，C5 留在下载/stub 会话。
- 目前未发现 Flash 内容损坏或修订号不兼容，不能因此宣称完整 C5 硬件正常、
  Hosted 已运行、GPIO2 分时或免引脚 OTA 可用。下一步仍需独立应用执行证据，
  无 UART 的分层方案见 `docs/C5_NO_UART_TEST_PLAN.md`，尚未执行自定义 RAM/最小 Flash 程序。

证据：`firmware/p4_gesture/test-results/c5_read_backup_20261110.json`、
`c5_flash_inspection_20261110.json`、`c5_revision_facts_20261110.json`、
`c5_download_final_health_20261110.json`。备份只在本地忽略目录保留，不提交或上传。

### 2026-11-10：最小诊断应用已写入，未启动验收

03:07 已将 150688 B 独立 SDIO 诊断应用写入 C5 ota_0（0x20000），MD5 校验通过。
这次实际更改了 C5 应用区，前节“未写 C5”仅指前节的只读取证阶段。
本次不改 bootloader、分区、otadata、NVS 或工厂区；原 ota_0 完整恢复包已按备份生成，
恢复操作未验收。C5 GPIO2 仅输入、无上下拉，不与 P4 音频争用，不测试 GPIO 分时。

P4 USB/程序复位/电源保持/FAULT 检查通过，软件回归 99 项通过，实屏未目视复验。
当前 P4 programmer 已带最小 SDIO 测试，C5 仍留在下载/stub 会话；
等待用户确认解除三根下载接地后，才执行单次 C5 复位、身份/心跳/随机回显测试。
**写入校验成功不等于 Flash 启动成功，更不等于 Hosted、Wi-Fi 或 OTA 可用。**

报告：`firmware/p4_gesture/test-results/c5_minimal_flash_20261110.json`、
`c5_minimal_flash_health_20261110.json`。无 UART 方案和恢复入口见
`docs/C5_NO_UART_TEST_PLAN.md`、`tools/README.md`。

### 2026-11-10：解除三脚后的运行结果（未通过）

用户确认三脚解除后执行一次 C5 复位/1-bit 400 kHz 卡初始化，卡初始化成功，
但最小应用签名超时（APP_SIGNATURE / 0x107），有效样本 0、回显未执行。
没有追加 C5 复位或刷写；仅更新 P4 的原始采样能力，随后不复位 C5 地执行一次
被动观察：20 次 12 B 共享寄存器 CMD52 读取全部成功，全部全零。
SDIO 初始化仍会设置卡/CCCR，之后只读共享字节，不访问 FIFO、不清中断。

官方 C5 LL 映射源码已核对，未发现偏移错误；源码核对不能替代独立硬件读通路验收。
目前只能确认未观察到最小应用运行，不能从全零直接判断 ROM 模式、复位循环或芯片损坏。
下一步需独立启动/早期初始化证据，不是继续以 Wi-Fi 参数猜测根因。

当前 P4 ELF SHA `075eab726c249b96c089fb3d1f4798a2a6a4f610acf860bf0ee15c35b97d7fc2`，
USB/供电保持/同一 boot_id/FAULT valid=0/触摸进度检查通过，driver_err=0，实屏未目视复验。
软件回归 104 项通过。C5 Flash 保留最小诊断应用，原 Hosted 未恢复；GPIO6 未操作，
Wi-Fi/OTA/GPIO2 分时仍未验收。报告：`c5_minimal_run_20261110.json`、
`c5_passive_observe_20261110.json`、`c5_minimal_conclusion_20261110.json`，均在本地忽略的
`firmware/p4_gesture/test-results/`。

## 附：GPIO54 = 雷达 SOP0 = C5 CHIP_PU

雷达型号是 **IWRL6432**（AOP 封装为 IWRL6432AOP），启动模式由 SOP1/SOP0 两条线在
NRST 上升沿采样（SWRS298 Table 9-3 / SWRS323 Table 8-3）：

| SOP1 | SOP0 | 模式 |
|---|---|---|
| 0 | 1 | Functional：从 QSPI flash 加载应用并运行（参考设计默认） |
| 0 | 0 | Flashing：bootloader 空转，等 UniFlash 通过 UART 写 QSPI |
| 1 | 1 | Debug：绕过 bootloader，M4F 停住 |

所以 SOP0 确实是“下载雷达固件”用的那根线（拉低进 Flashing）。但它同时是 C5 的
CHIP_PU（低有效），因此：

- 正常运行必须 **SOP0 保持高**：雷达走功能模式，同时 C5 被释放。
- 只有刷雷达固件时才拉低，代价是 **C5 同时进复位**，刷完必须重新走一次 SDIO 连接。
- `bsp_radar_init()` 原先写的是 `SOP0 = 0`，等于每次 P4 上电都把 C5 按在复位里，
  直到 esp-hosted 的复位序列把它放开；现改为 `SOP0 = 1` 并加了
  `_Static_assert(PIN_RADAR_SOP0 == PIN_C5_RESET)` 防止两边定义漂移。
