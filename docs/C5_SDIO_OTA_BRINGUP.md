# C5 SDIO 免按键升级：新开发板接入与验收

更新：2026-11-10。旧板的运行结果不能转移为新开发板的实测结论。

**最新维护实现与待验收状态见 `C5_MAINTENANCE.md`。** 下文为出厂Hosted基线及历史记录；
已新增独立维护profile、UART有限范围首次安装、SDIO A/B工具和bootloader回滚，
156项离线回归通过；新开发板C5已完成首次维护基线安装及整片读回，P4维护固件/SDIO OTA/回滚尚待接入验收。
首次安装禁止使用merged覆盖全Flash；安装证据以维护文档为准。

## 目标与限制

1. 先检查现有 C5 Hosted；确有必要时才通过 UART 首次安装，不覆盖正常出厂固件。
2. P4 通过 SDIO 与 C5 完成 Hosted INIT、版本 RPC、心跳通信。
3. 后续上传 C5 **app**，由 C5 写非运行槽、完整校验、选槽、软件复位并重新握手；不用改 strap 或按开机键。
4. 分开验收固件失效时的恢复能力：需要确认硬件是否控制 C5 BOOT/EN，不能靠已失效的应用完成。

当前不承诺上述步骤已通，也未启用/验收启动失败自动回滚。双 OTA 槽本身不是回滚保证。

### 用户已明确的恢复要求

用户确认：开发板与目标板核心板同型，串口由用户手动切换；要求不仅正常 OTA，
还包括 **C5 应用无法运行时自动恢复**，之后核心板会迁焊到自制板。
下一阶段必须独立设计/验证受保护恢复程序或 bootloader 引导策略、握手/看门狗和失败回退，
不能用本轮 OTA handler 修复宣称已实现该目标。若 bootloader/恢复镜像本身也损坏，
能否进入 ROM 还取决于实际 strap/EN 控制硬件；当前未新增 GPIO6 或三根 strap 控制线。

### 后续进展：P4 已实际烧录，C5 未烧录

- 当前 COM4 经 ROM 确认连到 P4 v3.2，Flash16 MB；读取原应用头为厂商 CO5300 演示，非空白 Flash。
- 普通 UART RAM stub 无响应，改为 `--no-stub` ROM 直接读写，三段写入 hash 均通过。
- 新增 `firmware/p4_gesture/bringup` 无屏程序及 `tools/p4_bringup.py`；不套用自制底板外设逻辑。
  分区表地址/大小/类型与开发板读取值逐项一致，不整片擦除；原 factory 应用被替换，NVS/storage 不写。
- 运行镜像 ELF=`526f21d8381fa09767b409baaa59ef2acb880b2a9412b2239e3aff9292763046`，INFO/持续心跳已确认。
  首个烧录报告因 GBK 无法打印 UART 噪声而捕获失败，写入本身成功；修正工具后另行采集确认，未盲重刷。
  启动记录 reset=7(WDT) 不作隐藏，来源仍待定位；连续观测期未再次看到 task_wdt/panic。
- SDIO两轮：原状态 PROBE 和一次 GPIO54 低20ms/释放/等5秒后的 PROBE，均1-bit400kHz初始化成功，
  CIS完整读到 vendor=0092/device=6666；各5次12字节读取成功且全零，owner释放成功。
  未跑 Hosted 握手，不据此认定应用已运行、C5损坏或真实型号已经识别。
- 118项回归通过（新增5项P4工具保护测试），未给 C5 写入 Flash，也未发 C5 UART ROM 同步命令。
- 已请用户切到 C5 UART，暂不改三根 strap；先取启动证据，再确认 C5 下载。

本地报告：`new_devboard_p4_flash_20261110.json`（写入成功/首捕获编码失败）、
`new_devboard_p4_runtime_20261110.json`、`new_devboard_c5_probe_20261110.json`，
均在 `firmware/p4_gesture/test-results/`。本节更新优先于下方首次候选准备时的“尚未烧P4”历史状态。

### 最新证据：已读到出厂 C5 完整启动（2026-11-10 17:49–17:52）

- C5 v1.2、4 MB，ROM `esp32c5-eco3-20250704`，Flash 正常启动 `SPI_FAST_FLASH_BOOT`。
- `network_adapter` **ESP-Hosted 2.12.9**，IDF v6.0.2，构建于 2026-07-14；
  ELF 日志仅提供前缀 `830470ccd...`，没有获得完整 SHA，不伪造镜像身份。
- 应用从 ota_0=0x20000 运行；两个 0x1e0000 B 槽，另一个 ota_1=0x200000。
  UART 日志显示已初始化 SDIO stream、host reset handler，并持续输出到 uptime=150529 ms。
- 本轮接收窗口内有三个 POWERON 头，前两个镜像加载中断，最后一次完整启动后稳定输出；
  用户操作时序未独立确认，不能据此前两次启动认定自动重启循环。
- SDIO 流量为0符合当前 P4仅做原始卡探测、未执行 Hosted INIT/RPC 的状态，
  不能再用旧最小诊断应用的共享寄存器签名解释这块新开发板。
- 全程未给新 C5 写 Flash/上传 RAM stub/发送 ROM 同步/自动复位序列；只额外发过一次 CRLF/help，
  无响应不能证明 RX 接线损坏。当前不需要先烧 C5。
- 候选 2.12.13 的修订号/容量范围与该启动日志相符；这不等于运行兼容性、OTA或自动恢复已通过。
  下一步以单 owner 的无屏 P4 Hosted profile 测 INIT + 芯片/版本 RPC，实际验证与 2.12.9 的互通。

证据：`firmware/p4_gesture/test-results/new_devboard_c5_boot_20261110_174940.{log,json}`，
180秒捕获已完成并关闭COM4。保持出厂 C5 固件不动，已请求串口切回 P4。

## 历史：首次候选准备（后续状态以上文为准）

- 发现 COM4：CH343，VID:PID=1A86:55D3。只接收 115200 UART，未发送数据或自动复位序列。
- 收到双核 `main` 占用 CPU0、IDLE0 超时的 WDT 日志，疑似 P4；尚未确认串口路由或固件身份。
- 当前没有 C5 启动日志；开发板型号、P4 SDIO 脚位及 C5 reset/strap 控制接线待确认。
- C5 OTA handler 修复：无会话/未结束不能激活；首包失败终止；重复 BEGIN 释放旧句柄；空包/容量保护；
  END 完整验证后才可选槽；激活定时器失败返回错误并尝试恢复原启动槽；失败恢复不允许再擦候选槽。
- C5 增加 UART 启动 project/version/ELF SHA/reset/分区及初始化阶段日志；检查 Hosted 初始化错误。
- 113 项 unittest 通过，其中 OTA 测试实际编译执行生产 handler，35 种场景 × 2 种头校验配置，共 70 次。
  IDF 操作由故障注入 mock 替代，因此不是 Flash/SDIO/实机测试。
- C5 构建通过：app 1,311,296 B，单文件镜像 1,442,368 B。分区/镜像 checksum、附加 SHA 离线验证通过。
- **本次没有给新开发板的 P4 或 C5 烧录，也没有替新板备份 Flash。**

候选 app ELF SHA：`40359b563a15507159fca795d932185a4ed17475cdafc6b5ee1b945bd1f7000c`。
候选仅支持 C5 v1.0–v1.99、4 MB Flash；新板兼容性待确认。构建默认仍来自旧自制板。
本地完整清单：`firmware/p4_gesture/test-results/c5_hosted_candidate_20261110.json`。

## P4 Hosted 实机验收已通过（2026-11-10 18:33–18:37，当前）

用户确认C5已复位、UART已切回P4；COM4 ROM再确认P4v3.2/16MB后只烧P4。
bootloader/分区表/应用三段hash通过，运行ELF与上节候选一致；新C5仍是出厂2.12.9，未写Flash。

- 单次CONNECT：由Hosted请求一次GPIO54复位，1-bit/400kHz卡初始化通过，
  收到真实 `Received INIT event from ESP32 peripheral`，传输进入TX_ACTIVE。
- 实时RPC返回 `chip=esp32c5 chip_id=23 fw=2.12.9`；不是沿用UART推断或缓存READY。
- 首轮VERIFY的10组，加后续10轮VERIFY各10组，再加CONNECT的一组，
  共 **111组芯片+版本查询 / 222次RPC，全部error=0**。
  离线逐组核对run/sample完整性，每轮样本0..9无缺失，11个终态均passed=1。
- 末端P4 uptime=203298ms，跨采集单调增长，未观察到新panic/task_wdt或通信失败。
  启动reset=7仍记录，原因未定位；不能把本轮通过称为已修复看门狗。
- 本轮不用GPIO6、不改三根strap、不启用Wi-Fi/BLE、不写C5；当前UART留在P4，采集已关闭。
- 验收仅覆盖当前新开发板1-bit/400kHz身份RPC，不外推为40MHz、多小时稳定、
  Wi-Fi业务、SDIO OTA、死固件恢复或旧自制板验收。

本地报告：`firmware/p4_gesture/test-results/new_devboard_hosted_acceptance_20261110.json`，
原始证据为同目录 `new_devboard_hosted_flash_20261110`、
`new_devboard_hosted_handshake_20261110`、`new_devboard_hosted_stress_20261110` 的JSON/log。

**下一步无需用户再切串口或接下载脚**：先实现P4应用级SDIO OTA及重启身份检查，
实际需要写C5前明确通知用户。受保护恢复/bootloader失败回退要独立设计，尚未实现。

## 历史：Hosted 候选准备及切回P4请求（已经完成）

把 UART 从 C5 切回 **P4**，三根下载脚保持未接地，不用再次复位 C5。
准备独立 Hosted profile 后只烧 P4，先做一次 CONNECT，再做10轮实时芯片/版本 RPC。
原始卡 PROBE 与 Hosted 分成不同镜像/缓存，禁止在 Hosted owner 下另行接管 SDMMC 或直拉 C5 EN。
必须明确收到本次命令接受及终态 passed=1，不能凭缓存 READY/串口通畅算通过。
需要改 C5 时另行通知；优先考虑应用级 SDIO OTA，不能直接拿 merged 文件覆盖厂商启动区。

无屏Hosted候选已构建通过：app605792B，ELF SHA=
`e1bc3d02d6293f6019b38616bc64a8197af4ad3199871086d89d2c83971799f4`；
app checksum/digest及与raw分区表一致性离线检查通过，125项unittest通过。
**尚未烧入P4或执行实机Hosted握手**；目前等待用户确认UART已切回P4。
本地准备报告：`firmware/p4_gesture/test-results/new_devboard_hosted_ready_20261110.json`。

## 镜像不能混用

| 场景 | 文件 | 地址/选择 |
|---|---|---|
| 首次 C5 维护配置（路由确认后） | `tools/c5_provision.py`读取独立app/bootloader | 先整片备份，只写ota_1/bootloader/otadata，不使用merged |
| C5 运行时 SDIO OTA | `firmware/c5_hosted/build_c5/network_adapter.bin` | 不传整片偏移；C5 自动选非运行 OTA 槽 |
| P4 固件 | 按确认后的新板引脚/Flash/PSRAM 另建 profile | 不能用 C5 文件，也不能默认用旧板 profile |

分区表当前 ota_0=0x20000、ota_1=0x200000，各 0x1e0000 B。OTA 更新不能写 bootloader、分区表或 NVS。
GPIO2 高阻守卫保留，未用 P4 GPIO6 控制下载，也未修改任何 eFuse。

## 宿主工具和实机验收（待实现/执行）

- 单一 SDIO owner；版本/镜像身份检查后才能传输，不把卡初始化成功当 Hosted READY。
- 当前上游 WRITE RPC 无 offset/sequence；丢 ACK 或断链停止当前传输，重建会话后从 BEGIN 重新开始，
  不能重发未知是否落盘的 WRITE。后续工具要实现这一策略，当前只修了从机行为。
- 首包至少包含 288 B 镜像头及 app descriptor；成功的 END 代表 IDF 完整镜像验证，ACTIVATE 才修改启动选择。
- 激活后的“命令成功”不算验收：等待 C5 重连，核对新 ELF 身份及运行分区、心跳/版本 RPC/持续 SDIO 读写。
- 连续至少 10 次 A/B 交替升级；中断传输、坏镜像、重复开始、宿主复位、冷启动分别测试。
- OTA 期间断电/启动失败及回滚策略需单独设计和验证；先确认实际板型，不把 mock 测试当电源故障保证。
- 每轮保存本地报告；中断或失败不自动循环复位、不盲刷。备份/日志可能含凭据，不提交 Git。

## 本地回归

```powershell
python tools/meoave.py c5
python tools/meoave.py test
```

前者只构建/合成，不烧录；后者不打开硬件。OTA C 测试需 PATH 上有 native gcc/clang，
或设置 `MEOAVE_HOST_CC`；无编译器时会明确 SKIP，不能算已执行。
