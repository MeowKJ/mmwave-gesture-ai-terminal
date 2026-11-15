# Meoave 工具入口

## UI 资产（可选重生成）

普通编译/回归直接使用仓库的图标 C 数组和音乐 PCM，不需要下载或额外解码库。来源、SHA-256 和许可保留在 `firmware/p4_gesture/main/assets/`。
只有改源资产时才安装 `tools/requirements-assets.txt`，分别运行 `python tools/build_icons.py`、`python tools/build_preview_audio.py`；不要为此清空 ESP-IDF 构建缓存。字体文案变更仍走现有 `build_fonts.py`。

本轮小屏交互与离线/实屏边界见 `docs/UI_POLISH_20261113.md`。PowerShell 连续验证时必须在编译非零后立即停止，不能继续运行旧的测试二进制。

## 产品 Wi-Fi 扫描回归（2026-11-13）

`python tools/meoave.py build --profile product` 现在默认包含 Hosted Wi-Fi，一次性迁移旧离线配置，保留显示/触摸/电源/音频及构建缓存。构建核对最终 ELF 的 Wi-Fi 结构 ABI；不再手工加入原生 Wi-Fi 头文件路径。

`python tools/wifi_acceptance.py --cycles 3 --report firmware/p4_gesture/test-results/wifi-scan-01.json`

只走已核对身份的原生 USB，使用真实扫描代次及逐项 AP 回复；不刷机、不复位 C5、不发密码、不连接网络。丢失 SCAN 确认不重发变更命令，空结果不冒充射频验收通过。报告不覆盖旧文件。软件、桌面渲染、射频扫描、触摸和互联网验收分别记录；详见 `docs/WIFI_AUDIT_20261113.md`。

2026-11-13 自制板实测：只更新 P4 后，3 轮扫描分别收到 28/29/29 条 AP 记录，每轮提供 6 个不同 SSID，中文名称正常；全程未刷写或主动复位 C5。用户确认无花屏、列表/滚动/密码页正常。另只读观察到 ONLINE/DHCP；用户暂不追加连接测试，DNS/HTTPS 与长期稳定性未验收，不能据扫描通过宣称完整联网通过。

## 产品音频自动验收

`python tools/audio_acceptance.py --report firmware/p4_gesture/test-results/audio-acceptance-01.json`

只用已核对 VID/PID 和完整 ELF SHA 的原生 USB；复用 UI 真实设置/CC0 Kenney 拨弦音乐链路，测试 30%/0% 音量、防连点、麦克风进出和低音量声学回环。保留已校准的单端输入路由；不刷机、不复位、不操作 C5、不重新校准，失败不自动重发播放。只读状态允许有界重新查询。成功回到眼睛页并保留 30% 音量。报告要求新文件；串口成功不等于人耳音质/实屏触摸验收。

交互音诊断：`UI CUE` 走与界面相同的请求路径，不绕过开关、静音或输出互斥；`UI` 返回的 `MEO:UI_AUDIO` 包含 `cue_enabled/requests/played/error`。这些计数只能确认软件请求及驱动返回，不能替代人耳听感；R2 三次返回成功但用户仍未听到的结果必须保留。具体证据与当前验收见 `docs/UI_POLISH_20261113.md`。

2026-11-12：P4 两次下载及一次普通程序复位通过，试听/静音寄存器/防连点/麦克风进出子项通过，用户确认音乐听感正常、复位后无花屏。**声学回环未通过，整体验收报告仍为失败**；不能把 `controls_ok=true` 当作完整 PASS，见 `docs/UI_INTERACTION_20261112.md`。

## 新增：C5免引脚维护（开发板10轮升级及故障恢复验收通过）

- `c5_provision.py`：首次UART识别C5、整片备份、原厂分区/应用验证，只写ota_1、回滚bootloader和otadata。
  必须用户确认UART切C5，禁止merged/整片擦除；备份与报告保存在忽略的test-results。
- `c5_maintenance.py`：后续PC→P4UART→SDIO app-only升级；支持10轮升级、中断、未确认超时回滚、
  启动前故障镜像回滚、共享GPIO2租约弱拉/高阻测试；未知WRITE/ACTIVATE结果不重发。
- `p4_bringup.py --profile maintenance`独立编译/烧P4；不用于需要GPIO29保持的自制底板。
- `c5_maintenance.py reset-soak --port COM4 --cycles 20 --report <独立文件>`：只复位P4并重新ATTACH，
  每轮实时核对C5 boot nonce/完整ELF/VALID槽不变；panic即停并保存寄存器/回溯尾部，不重发写命令。
- 维护UART从选定构建的`config/sdkconfig.json`读取；最终profile使用自定义UART0 TX37/RX38、460800，
  P4 ROM烧录仍保持115200/no-stub。缺少配置或运行ELF不匹配时拒绝继续，不猜波特率/固件版本。
- `meoave.py c5 --fault-test --no-merge`独立故障镜像，绝不可作首次维护基线。
- UART备份默认460800，使用1KB单包在途/远端MD5；在发送读命令前固定timeout，避免Windows接收途中SetCommState造成截短。
  `--no-stub`提供ROM64B读/分块MD5对照，`--stub-version`仅选择官方已安装stub，不修改Python包。
- `meoave.py c5 --hang-test --no-merge`生成独立不启动Hosted/不自行复位候选；只用于`hung`验收，不能首次安装。
- 日常升级入口：`python tools/c5_maintenance.py update --recover-baseline --port COM4 --report <独立文件>`。
  上传前一次EN建立干净VALID基线；候选启动后ATTACH不复位C5。正常OTA不占用GPIO6/共享GPIO2。
- 具体链路、命令与实测边界见`docs/C5_MAINTENANCE.md`。173项离线测试及C5备份/有限区域安装/整片读回通过。
  用户已切回P4，实时维护RPC与GPIO弱拉/高阻租约通过；原P4双核profile偶发启动Store fault，
  延后Hosted初始化仍在第19次复位失败，之后另30次通过也不抹去该失败。
  单核测试未见该panic，但P4单独暖重连分别在第96/47轮失败；不能宣称100轮通过，需上传前基线复位规避。
  该隔离不是产品双核根因修复，不关闭PMP/栈保护/看门狗；SDIO OTA/回滚以各自实测报告为准。
- 2026-11-11实机：10轮完整升级/显式确认、4096B中断、90秒未确认回退、启动前失败回退、
  SDIO失联95秒后单次EN回退、弱拉租约及最终正常VALID身份均通过；无需人工切C5串口或下载三脚。
  **这是电脑工具协调的开发板维护验收，不是产品原生USB/音频仲裁集成完成，也不承诺bootloader/两槽都坏可恢复。**

## 自制板：原生 USB C5 维护（新增）

- `python tools/meoave.py build --profile maintenance` 保留产品显示/触摸/电源/USB，复用已测 UART 维护状态机。
- `python tools/c5_native.py check --port COM10 --report <新文件>` 默认只 ATTACH，不复位、不刷 C5。
- 显式 `--reset-c5` 才请求一次 EN；产品音频/雷达必须保持停用。无隐藏复位/重试。
- 已安装维护基线后才可 `c5_native.py update`；不能用它首次安装尚未运行的维护服务。
- USB 随机事务边界、完整回复及真实 C5 身份联合验收；GPIO6/PIN、NET 暂拒绝，不宣称产品音频/联网已集成。
- 详见 `docs/C5_NATIVE_MAINTENANCE.md`。不要把下一节开发板无屏 P4 固件用于自制板。

## 切换串口开发板：独立 P4 无屏测试

### Wi-Fi 实机验收入口

```powershell
python tools/p4_bringup.py --profile maintenance --build
python tools/p4_bringup.py --profile maintenance --flash --port COM4 --capture 8
python tools/c5_wifi.py --port COM4 --ssid "<SSID>" --recover-baseline --cycles 3 --report firmware/p4_gesture/test-results/c5_wifi_check.json
```

密码交互隐藏输入，或从`--password-env`指定的环境变量读取，**不支持密码命令行参数/配置文件**。
密码不编入P4/C5镜像，UART不回显；报告对明文及十六进制密码脱敏。Wi-Fi诊断禁止NVS持久化，
仅用RAM配置，结束时清空配置、断开并停止Wi-Fi，再验证C5仍为原boot/ELF/VALID/高阻。

- 先实时核对P4镜像、C5正常固件身份；`--recover-baseline`显式一次EN复位建立基线，不改下载脚。
- 由C5射频扫描目标SSID，P4接收真实DHCP事件；DNS与HTTPS在P4 lwIP上执行，**不是电脑联网结果**。
- HTTPS启用CA bundle、域名及证书有效期检查；时间来自当前电脑UTC epoch，**未测试独立SNTP校时**。
- `--cycles`重复DNS/完整HTTPS请求，不是重复刷机或重新关联热点。每轮至少一个目标完整验证通过，
  各目标失败原样记录；`all_targets_ok`另行指示是否两个目标都通过。无隐藏复位/重连来掩盖失败。
- 该入口只增加P4诊断能力，不刷C5；真实结果以各次独立报告为准，不以离线测试替代。
- 2026-11-11家用AP实测：目标扫描1个、channel8、RSSI -92dBm；关联收到reason4断开，未获IP。
  调整P4启动/双频调用顺序后的报告为`c5_wifi_acceptance_20261111_r2.json`（false），清理和C5身份保持通过。
- 后续手机热点报告`c5_wifi_hotspot_20261111.json`为true：C5扫描RSSI -71dBm、关联 -67dBm、DHCP 172.20.10.3；
  example连续3轮DNS/HTTPS 200、577B完整响应、证书校验通过。espressif返回302/0B，不计严格通过，`all_targets_ok=false`。
  未重新编译或刷P4/C5；联网期间C5同boot/ELF/VALID/HIZ，结束清空RAM凭据并断开Wi-Fi/释放UART。
  基础真实联网已验证，不代表家用AP问题已修复、持久在线配置或长期稳定性通过。

`p4_bringup.py`只适用于已确认的P4v3.x/16MB开发板；不要把此固件下载到需要GPIO29保持的自制底板。
`--profile raw`是原始卡身份探测，`--profile hosted`是独立Hosted INIT/实时RPC验收，两者构建缓存分开。

```powershell
python tools/p4_bringup.py --profile hosted --build
# 用户确认UART已从C5切回P4后才能执行下两行；不写C5 Flash
python tools/p4_bringup.py --profile hosted --flash --port COM4 --capture 8
python tools/p4_bringup.py --profile hosted --port COM4 --capture 5 --command CONNECT --command VERIFY
```

CONNECT含一次明确的C5复位/卡初始化，VERIFY做10轮实时版本/芯片查询；失败停机不盲重试。
工具只在匹配本地P4 ELF后发操作命令，拒绝跨profile接管SDIO；缓存READY不是实机验收。
新开发板C5现有出厂Hosted2.12.9已确认，不需要预先刷merged镜像；完整边界见`docs/C5_MAINTENANCE.md`。
这不是SDIO OTA上传工具，也未实现C5应用失效恢复。

所有机器相关路径放在未纳入 Git 的 `tools/config.toml`，不要把本机用户名、ESP-IDF
安装路径或虚拟环境路径写进源码。第一次使用：

```powershell
Copy-Item tools/config.example.toml tools/config.toml
python tools/bootstrap.py
python tools/meoave.py doctor
python tools/meoave.py all
```

常用命令：

```powershell
python tools/meoave.py build                  # 产品固件（秒级增量编译）
python tools/meoave.py build --reconfigure    # 强制重新执行 CMake 配置
python tools/meoave.py build --profile hosted # P4C5 SDIO Hosted 静态构建
python tools/meoave.py c5                    # C5 Hosted 从机镜像
python tools/meoave.py build --profile programmer # P4 手动-entry SDIO 烧录器
python tools/meoave.py c5-program --install  # 安装烧录器、识别 C5，尚不写 C5
python tools/meoave.py c5-program --flash    # 写入四段 C5 镜像并逐段 MD5 校验
python tools/meoave.py c5-program --facts    # 读取 C5 芯片修订号/启动寄存器，不写 Flash
python tools/meoave.py c5-program --install --reset-download --facts # 保持三脚接地；雷达关闭时仅复位 C5 一次
python tools/meoave.py c5-read --install --output firmware/p4_gesture/test-results/c5-backup.bin --report firmware/p4_gesture/test-results/c5-read.json # 只读备份；仅更新 P4
python tools/meoave.py c5-test --build --install # 一次编译/烧录 P4，自动跑 C5 SDIO 矩阵
python tools/meoave.py c5-test --case 1:400 --case 4:5000 # 已装匹配固件时直接复测
python tools/meoave.py c5-test --case 1:400 --init-ms 20000 # 延长 INIT 观察窗口，不重刷
python tools/meoave.py c5-test --case 1:400 --init-ms 20000 --poll-ms 100 # 与中断模式对照
python tools/meoave.py test                  # 主机回归测试
python tools/meoave.py all --hosted           # 产品 + Hosted + 测试
python tools/meoave.py hardware --build       # 原生 USB 下载/复位验收
python tools/meoave.py clean                  # 仅删除两个生成目录
python tools/meoave.py fonts --fetch          # 下载字体源包并校验 SHA-256
python tools/meoave.py fonts --set round      # 切换字体集并重生成 LVGL 字库
python tools/meoave.py fonts --list           # 查看可选字体集
```

字体源文件不进 Git，默认从本仓库的 `fonts-v1` Release 下载
（`meoave-fonts-v1.zip`，75 MB，含六套 OFL 字体与逐文件校验和）；
`--fetch` 会先走直链，失败再走 GitHub API 资源地址，断点续传并校验 SHA-256。
改文案要增删汉字时，先补 `firmware/p4_gesture/main/fonts/manifest.json` 的符号集再重生成。

原自制板硬件流程只使用主板原生 USB，禁止 COM3/WCH-Link；首次冷启动仍需要板上开机动作。
Hosted profile 不代表 C5 已烧录或 Wi-Fi 已联网，GPIO6 保留给音频，GPIO54 与雷达 SOP0
的共享复位风险仍需实机验证。

2026-11-10 新接入的可切换 P4/C5 UART 开发板另行确认硬件，用户已授权该开发板串口；
不要把旧板禁用 COM3 的约束误套到新 CH343/COM4，也不要给未知板型直接运行旧板 `hardware`。
当前 C5 OTA 候选与待做实机步骤见 `docs/C5_MAINTENANCE.md`。`c5` 只构建，不代表已烧录。

开发板独立无屏 P4 程序（不写 C5；不能替代自制底板产品固件）：

```powershell
python tools/p4_bringup.py --build --flash --port COM4 --capture 10
python tools/p4_bringup.py --port COM4 --capture 8 --command PROBE
```

只能在串口已人工切到 P4 时使用；工具强制 P4芯片检查、ROM no-stub下载和运行ELF身份核对。
`--command C5RESET` 是显式单次C5 EN请求，用户切换到C5 UART后不要继续向该口发P4命令。
详情见 `firmware/p4_gesture/bringup/README.md`。软件“ok”表示工具执行完毕，不是Hosted/OTA验收通过。

## C5 自动化分层测试

`c5-test` 默认比较 1-bit/400 kHz、1-bit/5 MHz，以及 4-bit 的 400 kHz、
5/20/40 MHz。测试配置仅在当前启动生效；不改默认配置，不为每组重新编译。
`--build` 是一次增量编译，`--install` 是一次 P4 原生 USB 烧写/校验，均不写 C5 Flash。

- 每组先验证 P4 程序复位、电源保持和镜像身份，再发一次 CONFIG、一次 CONNECT。
- 每组通过 Hosted 自己的 SDIO 控制器，显式复位 C5 一次并仅初始化卡一次；
  初始化成功后默认等 4 秒 Hosted INIT 事件，不用内部重试掩盖不稳定。
  `--init-ms` 可设为 4000..20000（200 ms 步进）；这是轮询计时的名义窗口，寄存器采样有少量开销。
  `--poll-ms 0` 使用正常 DAT1 中断，`100` 仅在诊断时改用同一个接收任务每 100 ms
  检查寄存器/FIFO，不新建 SDIO owner。两种模式每秒保存一次只读 CMD52 快照，
  不读取 FIFO、不清中断；没有快照或读取失败不得解释成“从机数据全零”。
- GPIO54 同时连接雷达 SOP0，工具必须确认 radar=0；GPIO6 不配置、不驱动。
- Hosted profile 阻止旧 PINS/PROBE/PIN54 命令：即使 IDLE 也不能争抢控制器/引脚。
- ACK 丢失只查询状态，不重发变更命令；必须匹配本次 nonce、operation 和速率/位宽。
- 握手失败但 P4 健康时继续下一组；USB/供电/触摸/故障记录/完成状态异常立即停止，
  不自动补刷或盲目复位。每组写 JSON 检查点，全部完成后程序复位到默认 IDLE。
- READY 必须已有真实 C5 型号/固件版本及 Wi-Fi start；卡初始化通过本身不算 READY。
  短时 READY 稳定检查不等于联网、吞吐、OTA 或 GPIO 分时验收；实屏目视另验。

报告默认在 `firmware/p4_gesture/test-results/c5_automated.json`，不进 Git。
退出码 0=全组 READY 且健康检查通过，2=矩阵完成但存在握手失败，1=工具/健康保护中止。

## C5 Flash 只读备份

先构建 `build --profile programmer`，将 C5 手动置于 SDIO 下载模式并保持；
`c5-read --install` 只更新 P4，不写 C5。工具连接后上传官方 RAM flasher stub，
先重复读取 256 字节，成功才分块备份整个 4 MB Flash，并重复核对前 128 KB。
`--limit 0x20000` 可只读启动区；这不算整片备份。

每块 4096 字节，库校验目标返回的数据 MD5；USB 分片另核对 P4 缓存 MD5，
完成后校验磁盘长度/SHA-256。命令丢 ACK 只查缓存状态，不重发 READ；
DATA 仅查询已完成缓存，允许有界重查。失败不复位、不擦除、不自动重刷。
不覆盖既有备份/`.partial`；未完整验证的输出始终保留 `.partial`。
报告和备份放本地 `test-results`，其中可能含凭据/工厂信息，不提交 Git 或上传。

`c5-program` 默认仍不复位 C5。只有显式 `--reset-download` 才发一次 BOOTCONNECT；
此前必须确认 P4 供电、镜像和 UI radar=0，从新启动的 IDLE 状态执行。
该动作通过共享 GPIO54 拉低 10 ms 再释放并等待 5 秒，要求用户保持三脚下载接地；
不是失败后的恢复重试。不写/驱动 GPIO6，也不改三根 strap 或 eFuse。
`--facts` 要求本轮先观察到 IDLE，再完成新的 CONNECT/BOOTCONNECT，才接收其中采集的
C5 eFuse 修订字段和启动寄存器。若已经 CONNECTED/DONE，拒绝拿旧缓存冒充新测量，
不会自动重启或重连。USB `C5 FACTS` 本身只取 CONNECT 缓存，更换 token 不能刷新硬件；
Python `read_facts()` 的低层返回也明确标注 CONNECT_CACHE / live_gpio_read=false。
报告 `fresh_connection_capture=true` 仅说明本轮有新的连接采样，不代表持续实时测量。
这些寄存器不等于上次 Hosted 应用的启动日志。BOOTCONNECT 后须确认 reset_completed=1。

备份完成后可离线检查，不打开设备端口：

```powershell
python tools/c5_inspect.py --backup-report firmware/p4_gesture/test-results/c5-read.json --report firmware/p4_gesture/test-results/c5-inspect.json
```

该检查使用 esptool 的 C5 镜像解析器以及本机 IDF 分区解析器，显式校验镜像 checksum/
SHA-256、分区及 otadata；启动分区只给离线预测，不代表 C5 已执行。
可添加 `--historical-report` 比较原四段镜像的历史 payload SHA，
或 `--facts-report` 检查实际芯片修订号与镜像要求是否相容。

## C5 最小 Flash 应用测试（不等于 Hosted/Wi-Fi）

只支持已备份/验证的 ota_0 布局；先保持下载三脚接地，再依次运行：

```powershell
python tools/meoave.py c5-minimal --prepare
python tools/meoave.py c5-minimal --flash --reset-download --report firmware/p4_gesture/test-results/c5_minimal_flash.json
```

`--prepare` 离线核验备份/芯片版本，构建独立 SDIO 应用，同时生成原应用完整恢复包，
并增量构建嵌入诊断 payload 的 P4 programmer。默认输入为本机 2026-11-10 的已验证备份、
FACTS 报告；其他批次使用 `--backup-report`、`--facts-report`、`--work-dir` 明确传入。
`--flash` 安装 P4 后仅写 C5 ota_0 应用区、逐区 MD5 校验，不写 bootloader/分区/otadata。
`--reset-download` 先检查 radar=0，显式复位 C5 一次以重采样下载 strap，不是失败后的重试。

**写入校验成功后，告知用户解除三根下载接地；明确收到确认才执行：**

```powershell
python tools/meoave.py c5-minimal --run --straps-released --report firmware/p4_gesture/test-results/c5_minimal_run.json
```

该步骤先释放下载器 owner，显式复位 C5 一次，以 1-bit/400 kHz 验证真实固件 ELF SHA、
20 次心跳采样和 16 个 64 字节随机挑战回显。失败即停，不重刷、不循环复位；
签名缺失仍不能判定芯片损坏。P4 电源/USB/FAULT/触摸进度另验，实屏需要目视。

需要恢复原应用：用户重新将三脚接地后，再显式执行：

```powershell
python tools/meoave.py c5-minimal --restore --reset-download --report firmware/p4_gesture/test-results/c5_minimal_restore.json
```

恢复命令先检查恢复内容与原备份逐字节一致，再构建/安装 P4 恢复 programmer，
单次 C5 复位、只写完整原 ota_0、MD5 校验。`--build-restore` 仅构建恢复 programmer，不碰硬件。
切换 payload 后，运行中 P4 必须与对应本地 build SHA 一致，不能混用普通 `c5-program`
默认 Hosted payload。恢复写入成功后仍需解除三脚并安排受控启动；本工具不会自动猜测引脚状态。

所有 staging、恢复镜像与报告在本地忽略的 `test-results`，不提交/上传。

### 最小应用失败后的原始采样（不重试启动）

`c5-minimal --observe --install` 只安装已经构建的 P4 programmer，不改写/复位 C5。
**先用 staging 中同一个诊断 payload 增量构建 P4，不能重新 prepare 生成另一版 C5 身份，
也不能把普通 Hosted payload 对应的 programmer 混用。**

```powershell
python tools/meoave.py c5-minimal --observe --install --report firmware/p4_gesture/test-results/c5_passive.json
```

该模式单次 SDIO 卡初始化（包括必要协议/CCCR 配置）后，采集 20 份原始共享字节，
不访问 FIFO、不清中断、不发回显、不复位 C5、不写 C5 Flash。每个 CMD52 读失败即停止；
缓存保留实际成功字节数、错误码和时间，不能把未读完的缓存零填充当成读到全零。
`DIAGTRACE` 只查询缓存，丢回复可有界重查，不会重新采样或触发复位。

`OBSERVED / ok=true` **仅表示采样操作和 P4 健康检查完成**；应用签名/心跳/回显是否通过
必须看独立运行验收报告，不能将该状态转成 Hosted/Wi-Fi READY。运行失败时顶层复位状态
也会保留设备实际报告的 reset_completed，不再因为签名超时误留 once_requested。
旧版本不提供 TRACE 时，保留 trace_error，不因此重置/重刷设备。

### 复位反馈与当前启动脚取证

新的 `DIAGRUN`/`BOOTCONNECT` 会缓存 `C5 RESETTRACE <token>`：GPIO54 的初始高、
低电平前后、释放后高电平及末端电平、低电平采样间隔、IO MUX/输入输出配置。
写电平后先留 1 ms 稳定时间，不将紧随写入的旧同步读数当成拉不动；不改驱动强度或解除 hold。
`reset_completed=1` 仅说明 P4 pad 检查完成，不能冒充 C5 EN 引脚波形/应用启动证明。
缓存查询不重复脉冲，失败不自动重启 C5。

下载连接的 `--facts` 另读 C5 GPIO_IN/GPIO_ENABLE 与 IO MUX26..28，只读固定白名单，
不改变 C5 启动脚或 GPIO2。报告分别保存当前输入与启动锁存值；输入 buffer 未启用时
level=null（未知），不能把寄存器零值当成真实低电平。
无 `--reset-download` 的 CONNECT 不发送 C5 复位命令，但会上传/执行官方 RAM flasher stub；
因此只读 Flash/寄存器不等于不改变 C5 RAM。不可在取证后仍声称保留原应用 RAM 现场。

## 句子语音／表情（2026-11-13）

- 目标、协议、TTS 选型与分层验收：`docs/AI_VOICE_PLAN.md`。
- `python tools/voice_gateway.py validate --plan tools/voice_plan.example.json`：无依赖、无网络，逐句校验每句非空表情链。
- `voice_gateway.py synthesize`：在独立 IndexTTS Python 环境中生成 PCM＋设备 JSON；本机路径／模型／音色配置在私有 `config.toml [voice]`。不是 WSS／ASR／LLM 服务，不自动下载模型。
- 生产语音验收使用 Go 服务端的 `/v1/play`，通过 `VOICE OPEN` 和带 CRC 的 META/PCM/END 帧联调；不使用内置测试音频或固定 demo。
- 软件门禁：`ctest --test-dir firmware/p4_gesture/tests/desktop/build -R 'conversation\.|ui\.|model.state|audio.adapter' --output-on-failure`；网关测试为 `python -m unittest discover -s firmware/p4_gesture/tests -p test_voice_gateway.py -v`。
- 串口/DMA 通过不能替代用户听感／实屏验收；真实 GPU TTS、P4 WSS、录音／识别尚待接入。

### 试听音乐素材电平（2026-11-14）

`build_preview_audio.py` 使用已锁定的 CC0 原素材，生成峰值 22000 的 24 kHz PCM16（约 3.5 dB 采样余量），同时更新素材 manifest 的 PCM SHA／RMS／生成依赖版本。拒绝空白、非法或不符合电平约束的输出；不是提高全局音量或修改功放寄存器。`test_ui_assets.py` 与实际 C renderer 的 `audio.preview` 检查源完整性、响度电平下限、峰值上限、立体声、时长和静音尾部。实机修订范围与听感验收见 `docs/AUDIO_LOUDNESS_20261114.md`。

### Edge-TTS 网络延迟探测（2026-11-14）

用户确认无 Azure Speech 凭据，允许测试 Edge-TTS。此工具不是 Azure 正式 API、生产语音网关或设备端对话测试；不访问麦克风、USB、C5，不改固件／音量。

```powershell
python -m venv tools/.venv/tts-bench
tools/.venv/tts-bench/Scripts/python.exe -m pip install -r tools/requirements-tts-bench.txt
tools/.venv/tts-bench/Scripts/python.exe tools/benchmark_tts.py --out test-results/tts-new-run --repeats 3 --timeout 20
python -m unittest discover -s firmware/p4_gesture/tests -p test_tts_benchmark.py -v
```

仅发送工具内三条合成测试文本，逐次请求、每次有总超时，两次连续失败停止；无应用层重试，SDK 内部行为计入耗时。首包只统计非空 audio 事件，另记最后音频与 stream 完成时间；落盘／MP3 检查在计时外。输出目录必须是新的，保留旧报告。报告与 MP3 位于 Git 忽略目录。

本机首轮 9/9 合成收到有效音频：短／中／长文本首音频中位数约 1250／1406／1245 ms，stream 完成中位数约 1585／1881／1778 ms。仅代表本机当次网络，不含 ASR、LLM、设备传输、解码或播放；三次样本不建立 P95 或长期可靠性。设备当前完整句子协议不能直接以首音频时间宣称可开口时间。证据见 `test-results/edge-tts-latency-20261114/run-01/report.json`。

## ESP-SR 声学唤醒（2026-11-14）

目标与真实验收边界见 `docs/ESP_SR_WAKE.md`。`tools/config.toml [features].espsr=true` 启用官方 ESP-SR；`[espsr].diagnostic_preset="hi_miaomiao"` 为用户已同意的临时 Hi，喵喵 词，默认正式词仍为猫猫。定制资产使用本机 model_dir/model_name；验证元数据与 SHA，修改资产只使模型包失效，不无故触发全量重编。

`python tools/wake_acceptance.py --listen-seconds 60 --min-detections 1 --report <新的绝对路径.json>` 只通过原生 USB 观察真实采集/推理进度与声学事件，返回眼睛页；不发文字唤醒、不播放音频、不复位、不访问 C5。默认 min-detections=0 仅验证软件链路；没有声学事件时仍明确标为待外部人声验收，不能声称已识别。
## 免人工声学唤醒验收（2026-11-15）

```powershell
python tools/meoave.py wake-acoustic --flash --port COM10 --report firmware/p4_gesture/test-results/新的唤醒报告.json
```

- `--flash` 可选：复用已验证的 P4 原生 USB 下载与 CPU-only 启动，先校验四段 Hash，再核对运行 ELF/boot/供电；不重置 C5、不切下载脚、不触碰 COM3。
- 默认先枚举并选择本机已启用的中文 Windows SAPI，离线生成“Hi，喵喵”与不含关键词的负例；这是测试音源，不是替换 Go/Edge-TTS 产品服务端，也不是“猫猫”定制模型。
- 电脑默认扬声器→空气→板上真实麦克风→ESP-SR；不通过 USB 注入音频或 `AI WAKE`，不取消设备自身扬声器的防误唤醒保护。**板子必须已经上电、接着原生 USB，且处于电脑扬声器的可听范围。** 主机音量/默认音频设备均不擅自改动；静音或输出端点太远会真实失败，不伪造通过。
- 播放为异步，USB 持续采样，避免错过 1.8 s 确认；每轮检查独立声学计数／事件／来源、无文本注入和好奇表情。2026-11-15 起监听标志改为输入电平驱动的微型胶囊填充：声源播放时可见、填充序号增长，静默结束必须隐藏；不再要求安静时常驻波形，唤醒确认也不能强制点亮麦克风。
- 默认静默/非关键词负例、3 次正例、轮间冷却、420 s 持续推理；最终要求同 boot、正确 ELF、干净 fault、回到监听态。只允许有界的加载/模型重建暂态，永久失效/停推理/复位均失败。
- `--wav 正例.wav --negative-wav 负例.wav` 可用自备单/双声道 PCM16 WAV，跳过 SAPI；每段 0.1–15 s。`--repeat 1..10` 控制轮数；`--soak-seconds 0` 只做短冒烟，不构成 7 分钟稳定性验收。
- `queue_drops_delta/warnings` 单独报告队列掉块；连续推理恢复不等于零掉块通过，也不能将 7 分钟窗口推广为长期可靠性。
- 报告及音源 SHA/WAV 在忽略的 `test-results` 中，必须使用新文件名；每阶段持久化，失败保留烧录和局部采样，不自动重刷/复位重试。
- **软件回归、真实声学、LVGL 对象遥测、物理屏幕是不同验收层。** 没有摄像头时，不把遥测或桌面截图说成已目视检查实屏；该工具也不代表真实人声、距离/噪声识别率、ASR/AI 对话全部完成。

详细本次结果与历史失败见 `docs/ESP_SR_WAKE.md`。
