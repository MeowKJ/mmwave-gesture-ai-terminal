# C5 免引脚维护与共享 GPIO 分时链路

## 状态（2026-11-11）

**本开发板限定范围的免引脚维护验收通过：10轮完整SDIO升级、中断、超时/启动失败/失联恢复及GPIO弱拉租约。**
这不是产品P4集成、任意故障救砖、断电保护或长期稳定性全部验收。
用户已确认UART切回P4。C5为v1.2（revision102），安全启动/Flash加密未启用。
此前111组/222次出厂身份RPC不等于新维护固件验收。

- P4 maintenance、C5正常维护、C5故障注入三个独立镜像编译成功。
- 173项 unittest 通过（最新日志`c5_maintenance_tests_20261111_hang_r2.log`）；其中生产OTA C处理函数36场景×4配置=144次，
  生产维护C决策函数16场景。均为native/mock，**不是实机测试**。
- 本地镜像身份：`firmware/p4_gesture/test-results/c5_maintenance_ready_20261110.json`。
- 构建/测试日志：同目录`c5_maintenance_build_20261110.log`、`c5_maintenance_fault_build_20261110.log`、
  `p4_maintenance_build_20261110.{log,json}`、`c5_maintenance_offline_tests_20261110.log`。
- P4维护镜像已安装，首次运行和复位测试发现偶发启动Store access fault，不能报稳定通过。
- C5实时RPC：esp32c5/chip_id23/fw2.12.13、ota_1 VALID、完整ELF匹配。
  `c5_maintenance_handshake_20261110.json`为实时证据，不是UART安装后的推断。
- `c5_maintenance_pins_20261110.json`：UP/DOWN弱拉两端数字读回及500ms租约到期高阻通过；不是电压/音频测试。
- 首次完整SDIO升级`c5_sdio_update_first_20261111.json`为true：1316640B完整ACK，
  从0x200000写到0x20000，新boot nonce，PENDING_VERIFY→显式确认VALID；无失败补救。
  该次升级已正常覆盖原厂ota_0应用槽；原厂完整Flash备份仍保存在本地。

### P4启动问题（保留失败证据）

- 原维护P4 ELF `ac09db8a6f9f529a35ef264dc43f556b2df900e98bcb64bdcec9ed705ecadbae`，
  在进入app_main前填充FreeRTOS任务栈时，ROM memset产生Store access fault。
  `p4_maintenance_flash_20261110.json`、`p4_maintenance_reset_soak_20261110.json`以及
  `p4_startup_repro_20261110.json`保留实际失败，不用自动重启后的成功覆盖。
- 已直接读P4v3.2 ROM相关指令，确认故障PC处为对任务栈的store；底层芯片/总线根因未确定。
- 维护profile改为调度器/event loop启动后显式初始化Hosted，产品profile不变；
  不关闭PMP、硬件栈保护或看门狗，不更改CPU频率。早期360MHz配置实验因修订依赖未生效，
  不能把对应build文件名当成实测360MHz，且没有烧录该实验镜像。
- 延后初始化的115200双核镜像ELF `7ceda69033bc627fe081d577518378b3fbd24a9ef2e40dc87e2d1522dcb5c118`，
  20轮计划在第19轮同类启动panic停止（18轮通过）。随后独立30轮通过，**不抵消前次失败**。
  报告`p4_deferred_115200_reset_soak_20261110.json`、`p4_deferred_panic_detail_20261110.json`；
  后者名称为诊断目的，不代表该批捕获到panic。
- 因此另设维护专用`CONFIG_FREERTOS_UNICORE=y`进行隔离验收；仍400MHz、保留PMP/栈保护/看门狗。
  它不是产品双核根因修复，不能据专用profile通过外推产品profile已修复。
- 维护UART已统一460800（ROM烧录仍115200），工具从当前构建config读取实际波特率；
  自定义UART0明确沿用TX37/RX38。必须以最终生成config与实时身份验收，不能只看defaults。
- `reset-soak --cycles N`只软件复位P4，逐轮实时核对C5 boot nonce/完整ELF/VALID槽不变，
  发现panic、看门狗或C5意外重启即停止，不以反复复位掩盖故障。

### P4单独复位重连的已知限制与规避

- 单核首版100轮计划：前95轮P4复位/ATTACH通过，第96轮版本RPC超时；没有P4 panic。
  报告`p4_maintenance_unicore_reset_soak_20261110.json`保留false。
- 增加CP_INIT事件就绪门后再测：前46轮通过，第47轮丢弃stream包、未收到INIT；没有P4 panic。
  报告`p4_maintenance_rpc_ready_soak_20261110.json`保留false。不能宣称100轮通过。
- 当前安装P4 ELF为`e8b5f673e534099934fd6b774a04f6c56d44835b175381f8f3357cb0e7593707`。
  主机每次启动将SDIO收发计数清零，而未复位的C5仍保留累计计数；结合失败快照，
  **推断**暖重连存在计数不同步/旧FIFO数据问题。CP_INIT等待不是该问题的根治。
- 采用可审计规避：上传前显式`--recover-baseline`，仅一次P4软件复位+CONNECT/C5 EN复位，
  然后实时核对旧固件VALID、运行槽等于启动槽、引脚高阻。此操作可能回退此前未确认的候选。
  不偷偷塞入只读check，也不在新候选PENDING阶段复位C5。
- ACTIVATE使C5自行重启后计数重新开始，P4软件复位后ATTACH、不脉冲C5 EN，保留候选健康确认窗口。
  整个维护过程无人工绑带/串口切换，但**不等于无限次P4暖重连已经修复**。

### C5安装实测（2026-11-10，460800波特率）

- 原厂整片4194304B备份落盘校验，SHA256=`47c6e278474f5288dc50bff80b64cb4ad76fbde03e7f11e91a2260c9c1ef9037`。
- 原厂ota_0完整镜像校验通过，ELF=`830470ccde10eb3499341d297628204f639d57d357dcdf415da3f9c054b85eb4`。
- 实际只写ota_1应用1316640B、bootloader24208B、otadata8192B；三段工具hash及独立读回均通过。
- 再次读完整4MB并逐字节比对，所有允许擦写扇区之外的数据保持不变，包括原厂ota_0、分区表、NVS、证书与标定。
  这是**重启前Flash读回结果**，不替代后续运行态测试。
- 新C5 ELF=`b97127f15c5c60ab3a3981601b345a427c050eebc7ca09b709d5df0adaaff46f`。
- bootloader SHA256=`8586ed952e6fd7edc58e1ec7ce0440254714a4ec318e461411e9d5dc2aaa3e58`，
  对应构建回滚选项开启；真实回滚行为已由11月11日超时/启动失败/SDIO失联注入验收。
- 自动RTS重启后的只收UART截获到启动器选择0x200000并装载segment0；
  捕获脚本控制台GBK编码异常已关闭串口、保留967B原始数据，不能据此声称C5应用/Hosted已运行通过，也没有因此重复烧录。
- 实测报告及两份完整镜像：`firmware/p4_gesture/test-results/c5_uart_provision_20261110_fixed_timeout/`。

### 本次读取错误及修订

原工具在读命令后修改串口timeout；Windows pyserial会重新SetCommState。
460800与115200、默认4KB与1KB停等、两种官方stub均在首包出现截短，均在备份完成前停止，未写Flash。
ROM64B读取可前进，但速度慢，已在任何Flash写入前停止该对照。
修订为**发送读命令前**设置与esptool一致的timeout，接收过程中不再重新配置串口，
并使用1KB/单包在途及远端MD5校验。460800整片备份、烧录及完整读回已通过。
这一结果支持接收途中串口重配置为本轮问题触发因素；未据此认定排除了所有电气/驱动问题。

## 具体维护链路

```text
电脑 → P4 UART维护入口 → Hosted SDIO → C5当前运行应用
  → 上传前显式恢复一次旧VALID基线（P4软件复位 + C5 EN复位）
  → 查询实时芯片/版本/完整ELF/boot nonce/槽/OTA状态
  → BEGIN擦非运行槽 → 带本地offset+token逐块写 → END完整镜像验证
  → ACTIVATE选NEW槽并定时重启C5
  → P4软件复位（不复位C5）→ ATTACH（不脉冲C5 EN）
  → 新槽PENDING_VERIFY + 完整ELF匹配 + 连续健康查询 + 引脚高阻
  → 显式CONFIRM → VALID
```

- OTA仅写应用槽；不写bootloader/分区表/NVS/证书/标定。
- 首次重连不能使用CONNECT复位C5，否则PENDING镜像会在健康确认前被bootloader回滚。
- 上游WRITE没有远端offset/seq。因此每块512B停等一次应答，未知落盘结果不重发。
  本地token/offset不能伪装为远端幂等；失败停当前事务，下次新BEGIN重头写。
- 镜像checksum/附加SHA/chip/app descriptor/容量离线校验，宿主和P4都核对完整SHA与ELF。
- 正常启动但未确认：90秒后C5自行重启，bootloader回退旧VALID槽。
- 新应用在Hosted起来前崩溃：bootloader回退；挂死不重启/SDIO完全失联：
  PC维护工具最多一次P4软件复位+CONNECT/EN复位，随后核对旧VALID槽。
  恢复不重发WRITE/ACTIVATE，恢复失败即停止。
- 现阶段自动恢复由电脑维护工具协调，**不是产品P4后台无人值守维护服务**。

## 首次安装为何仍要切一次 C5 UART

出厂应用未实现新维护协议，出厂bootloader是否启用回滚未知。
因此一次性UART安装可审计的回滚bootloader和维护基线，以后应用升级不再要求拉下载三脚或切C5串口。

`tools/c5_provision.py`仅在明确路由C5后执行：

1. ROM识别C5/修订/安全状态，拒绝P4、安全启动/加密目标；不修改eFuse。
2. RAM stub + 1KB停等/远端MD5整片4MB备份，落盘fsync/哈希及读回。备份含敏感数据，不进Git。
3. 要求实机分区表与构建一致、当前选择ota_0，验证原厂ota_0完整镜像。
4. 按顺序只写：ota_1(0x200000)、回滚bootloader(0x2000)、otadata(0x16000)。逐段读回。
5. 再读完整4MB，与备份逐字节对比，除允许擦写扇区外必须完全保留。
6. 不写merged、不整片擦除、不动原厂ota_0/分区表/NVS/证书/标定。

首次UART安装将维护基线标记VALID，避免用户切回P4及烧P4时超过90秒导致回退。
这是**UART可信安装**，不是声称已通过运行健康验收；安装后仍需P4实时RPC验收。
之后所有SDIO升级必须经历NEW→PENDING_VERIFY→显式VALID。
bootloader写入期间断电可能需UART恢复；不承诺首次配置具备应用OTA级断电保护。

## “单独引脚”的处理

目前按已知共享网 **P4 GPIO6 ↔ C5 GPIO2 ↔ 音频DOUT** 实现。
它不是C5 GPIO26/27/28下载绑带，也不能凭改变GPIO2把死固件变成ROM下载模式。
GPIO54是另一条C5 EN/旧自制板雷达SOP0共享链路，不能混称。

```text
音频模式：C5 GPIO2高阻，P4/音频拥有该网
请求维护：停止I2S/DMA、静音/释放音频输出 → P4 GPIO6输入无上下拉
  → SDIO请求C5 GPIO2模式+租约 → C5先高阻再切换 → 数字输入回读
  → SDIO显式释放，或租约100..5000ms到期自动高阻
  → 验证C5高阻 → 才允许恢复音频
```

- 当前无屏维护P4根本不启动音频，因此只实现输入释放+SDIO请求；
  产品音频/I2S静音/停止/恢复尚未集成，不能直接复用为已验证的产品音频仲裁。
- 自动初验仅弱上拉/弱下拉/高阻，不在未知开发板网路上强推挽。
- C5其他通用GPIO RPC拒绝GPIO2，避免绕过租约。
- 数字回读不是万用表电压，也不能证明无模拟干扰。
- GPIO2租约不参与正常SDIO OTA；维护升级本身不依赖GPIO6。
- 租约释放依赖C5任务被调度；若整个C5挂死，需EN复位释放，不能声称5000ms硬实时保证。

## 实机验收记录（2026-11-11，UART固定P4，460800）

证据均在`firmware/p4_gesture/test-results/`，每项独立报告，不覆盖之前的失败报告。

| 项目 | 结果与严格判据 | 报告 |
|---|---|---|
| 正常升级 | 首次1轮+连续9轮全通过；每轮1316640B ACK、另一槽新boot/PENDING、完整ELF匹配、显式VALID，无失败恢复 | `c5_sdio_update_first_20261111.json`、`c5_sdio_update_nine_20261111.json` |
| 传输中断 | 只写4096B，不END/ACTIVATE；复位后旧槽/旧ELF/VALID保持 | `c5_sdio_interrupted_20261111.json` |
| 候选未确认 | 真实启动PENDING、故意不确认，90秒超时后回旧VALID槽，无额外失败补救 | `c5_sdio_unconfirmed_20261111.json` |
| 启动失败 | 故障镜像在Hosted之前自行复位，bootloader回旧槽/旧ELF/VALID，无工具EN补救 | `c5_sdio_fault_20261111.json` |
| SDIO失联 | 独立挂起候选上传1316800B；两次ATTACH明确报缺卡/总线错误，中间等95秒且只复位P4；随后一次C5 EN恢复旧VALID/高阻 | `c5_sdio_hung_20261111.json` |
| 引脚租约 | UP/DOWN两端数字读回一致，500ms租约到期HIZ；无强推挽/电压/音频测试 | `c5_sdio_pins_20261111.json` |
| 最终身份 | 实时RPC为正常维护ELF b97127f1…、ota_1/boot槽0x200000、VALID、GPIO2高阻/lease0 | `c5_sdio_check_20261111.json` |

后续测试顺序/退出码汇总`c5_sdio_followup_20261111.json`为true。
最终交叉断言汇总`c5_maintenance_acceptance_20261111.json`为true；另一次只读检查
`c5_sdio_final_health_20261111.json`在C5 uptime209072ms仍为相同boot/正常ELF/VALID/HIZ，无隐藏复位。
失联恢复后的C5 boot nonce为`7511f771`；不是继续运行故障/挂起镜像。
测试期间没有要求用户切C5串口、拉下载三脚、按复位键或开机键。
升级前基线EN复位与候选失败后的单次EN补救是不同阶段，报告分别记录，绝不把恢复旧应用冒充升级成功。

## 工具与复现命令

所有机器路径从`tools/config.toml`读取；以下相对命令在仓库根目录执行。

```powershell
python tools/meoave.py c5 --no-merge
python tools/p4_bringup.py --profile maintenance --build
python tools/meoave.py c5 --fault-test --no-merge
python tools/meoave.py c5 --hang-test --no-merge

# 必须先由用户确认UART已切C5；先备份后有限范围写入
python tools/c5_provision.py --port COM4 --uart-selected c5 --provision

# 上一步完成后用户切回P4；只烧P4，不动C5
python tools/p4_bringup.py --profile maintenance --flash --port COM4 --capture 8
python tools/c5_maintenance.py check --port COM4
python tools/c5_maintenance.py pins --port COM4
python tools/c5_maintenance.py update --recover-baseline --port COM4 --cycles 10
python tools/c5_maintenance.py interrupted --recover-baseline --port COM4
python tools/c5_maintenance.py unconfirmed --recover-baseline --port COM4
python tools/c5_maintenance.py fault --recover-baseline --port COM4 --image firmware/c5_hosted/build_c5_fault/network_adapter.bin
python tools/c5_maintenance.py hung --recover-baseline --port COM4 --image firmware/c5_hosted/build_c5_hang/network_adapter.bin
```

每个动作默认report路径相同；正式验收每步使用不同`--report`保留证据。
`--fault-test`使用独立build_c5_fault/sdkconfig，不污染正常镜像缓存；禁止把故障镜像做首次基线。
`fault`刻意在PENDING时于Hosted初始化前重启，必须观察回到旧槽及旧完整ELF才算通过。
`unconfirmed`故意不确认候选，必须在90秒超时后恢复旧VALID槽。
`interrupted`只传少量数据，不END/ACTIVATE，复位后旧VALID槽必须不变。
`--hang-test`在独立build_c5_hang中生成候选：PENDING时保持调度器运行，但在Hosted和90秒维护任务之前阻塞。
`hung`要求P4明确报告ATTACH的传输缺席错误，等待95秒超过正常90秒自回退期限，
仅P4软件复位后再次证明SDIO仍缺席，再最多一次EN复位，核对旧槽/旧完整ELF/VALID/高阻。
UART失联、P4 panic、任意其他错误不算注入成功；恢复已经尝试过即不重复EN。
这是“候选不提供SDIO且不自行复位”的测试，不模拟CPU/电源/EN硬件损坏。
挂起注入实机结果已通过，见上表；仅覆盖“SDIO服务不运行且不自行复位”的故障模型。

## Wi-Fi专项实测（2026-11-11，手机热点基础联网通过）

- 仅更新P4，当前维护+联网诊断ELF为`5261a5062454a7229bc1b6cdea82de7b1984bc6eb45e9b71d00baa323b51313c`。
  C5保持正常维护ELF b97127f1…，没有刷C5应用/bootloader，也没有操作下载脚。
  旧P4维护镜像已在忽略的`test-results/p4_maintenance_pre_wifi_20261111/`留存。
- 新增`tools/c5_wifi.py`和维护profile的NET_SCAN/NET_JOIN/NET_PROBE/NET_STOP。
  密码临时串口下发，仅Wi-Fi RAM配置；构建关闭远端Wi-Fi NVS，退出清空配置/停止/反初始化。
  报告脱敏明文/十六进制密码。HTTPS启用CA bundle、域名与有效期检查，时钟来源是当前PC UTC，不冒充SNTP验收。
- 首次扫描在P4调用顺序处返回0x3002（Wi-Fi尚未启动就设置双频），未进入连接步骤。
  已改为先start再set_band_mode，失败报告`c5_wifi_acceptance_20261111.json`保留，不覆盖。
- 修订后目标扫描通过：1个匹配，channel8、auth7（WPA2/WPA3混合）、RSSI **-92dBm**。
  init/storage/mode/start/band/PS API均返回0；连接返回断开reason4（IDF枚举：inactivity），没有GOT_IP。
  **不能据此断定密码错误或C5损坏，也不能宣称Wi-Fi联网/DHCP/DNS/HTTPS通过。**
- `c5_wifi_acceptance_20261111_r2.json`为false；清空RAM配置/停止/反初始化全部通过。
  测试后C5仍为相同boot `21a347ae`、正常完整ELF、ota_1 VALID、GPIO2 HIZ/lease0，无测试中重启。
- 187项离线测试通过（`c5_wifi_offline_20261111_r3.log`），含真实C解析边界、证书/持久化保护、
  报告脱敏、DNS/HTTPS验收假阳性拒绝；不是射频联网证据。
- 家用AP的-92dBm是排查线索，不是已证明的唯一失败根因；失败证据仍保留。
- 用户改用手机热点并授权查找名称：电脑先扫描得到完整SSID，仅作发现；随后由C5实际扫描、关联和获取IP。
  `c5_wifi_hotspot_20261111.json`为true，C5扫描RSSI -71dBm/channel8/auth3，关联RSSI -67dBm，
  DHCP IP 172.20.10.3/网关172.20.10.1；example目标连续3轮DNS成功、HTTPS 200、577B完整响应，证书校验通过。
  espressif目标3轮DNS/TLS成功，但HTTP 302/0B，按原严格判据不通过，`all_targets_ok=false`；未将302伪装成200。
- 本次未编译/刷写P4或C5、未操作下载脚。测试前显式一次基线EN复位；联网阶段初末C5均为boot 5a2fda8c、
  正常完整ELF b97127f1…、ota_1 VALID、GPIO2 HIZ/lease0。清空RAM配置/停止/反初始化全部通过，UART已释放。
  因清理步骤已主动断网，不表示热点凭据已保存或产品已配置；仅证明当前开发板链路基础联网能力，不是长期稳定性验收。

### 硬边界

- bootloader损坏、两槽都坏、EN/电源/SDIO硬件损坏，不能靠当前运行态SDIO协议保证恢复。
- 未做真实断电故障注入，不把mock或软件复位外推为断电测试。
- 已验证手机热点基础Wi-Fi/DHCP/DNS/HTTPS；BLE业务、高速SDIO、多小时稳定性及家用AP失败根因尚未验收。
- 此维护P4是新开发板无屏profile，无自制板GPIO29电源保持/nativeUSB/显示触摸。
  **不能直接把该P4镜像用于迁回的自制板**；需集成产品P4并回归供电保持、原生USB、显示暖启动及180°旋转。
