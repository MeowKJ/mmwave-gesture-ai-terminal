# ESP-SR 本地唤醒（更新至 2026-11-15）

## 本轮：自动声源与 UI 反馈验收

目标是无需用户说话、按键或切引脚，由电脑扬声器经空气进入真实麦克风。
一键入口及参数见 `tools/README.md` 的“免人工声学唤醒验收”。离线 SAPI 只用作外部测试音源，不改 Go 唯一服务端与产品 TTS。

### 实现与边界

- `tools/wake_acoustic_acceptance.py`：选择真实已安装的中文音色，或接受有界 WAV；异步播放同时采样原生 USB。静默/非关键词负例、3 个独立正例、轮间冷却、默认 420 s 持续推理；拒绝文本注入、来源不匹配、计数回退、推理停滞、模型永久失效、意外复位或 fault。
- 每轮要求独立声学计数/事件与 UI 代际对应，且好奇表情、`wake_detected=1`、实际 LVGL 琥珀提示同时被采到；波形序号必须增长。失败也保留烧录、试次及串口转录，变更操作不自动重发。
- `meoave_ui.c` 的首页分支原先让 `mic_tick()` 被 `else if` 跳过；现改为眼睛与麦克风波形独立更新。真实 LVGL 桌面回归检查波形随音量变化、停止隐藏、琥珀颜色，生成 `wake-listening` / `wake-ack` 预览。
- 原 app 每次拿不到 5 ms GUI 锁就令 `home=false`，会频繁停止/重新启用识别并重建模型。改用真实页面回调维护监听门禁，离开首页立即 disarm；渲染忙不等于离开首页。桌面 app adapter 回归覆盖“GUI 忙仍请求监听、实际离页关闭”。
- 不取消自身功放播放与 300 ms 尾窗抑制。不动 C5、GPIO6 音频、GPIO54、显示初始化/触摸/180°、GPIO29 供电保持。无摄像头时，LVGL 遥测/桌面预览不能证明物理屏幕像素已显示。

### 本轮实测结果（已完成，非长期可靠性认证）

- 新镜像 ELF SHA `db1009139a8a00f7813855ed16aef80aa6652a406af58078cca84cd64d829e86`；11 步增量构建，四段 Hash、CPU-only 自动启动、运行身份与供电核验通过。
- 首轮本机中文 `Microsoft Huihui Desktop / zh-CN`，真实声源正例 **3/3**，每轮各 1 个真实声学事件与 UI 确认；静默及非关键词负例均未触发，零文本注入。
- 420 s 连续推理及最终核验通过：同 boot `7224f6c6`，完整测试跨度约 458.26 s，`fault.valid=0`、`guarded=0`、`error=NONE`，推理累计 14245 chunk，零文本注入；未新增误唤醒。证据：`firmware/p4_gesture/test-results/wake-acoustic-20261115-v2.json`，音源及 SHA 在同名 `.audio` 目录。
- **保留性能警告**：长测约 t=441.5 s 时队列累计丢 8 个 10 ms 块，`resets` 从 7 增至 10 后恢复，结束仍 armed/推理持续；不能称为零丢块或长期识别率已验收。自动化报告 `ok` 表示所选窗口功能/连续运行条件通过，不等于调度性能完美；新工具显式输出 drops_delta/warnings。v2 是该提示字段加入前的原始报告，末段原始计数仍完整保留，未改写历史证据。
- 随后真实页面生命周期验证：离开首页后 `requested=0 mic_running=0 capture.visible=0`，返回后全部恢复 1；导航未重启。一次 `SYS RESTART` 无按键恢复原生 USB/供电/正确 ELF，故障干净，`wake-lifecycle-20261115.json` 留证。
- 程序复位后再用同一音源做 **3/3** 正例、静默/非关键词负例及 LVGL 反馈检查通过；两轮合计 **6/6**（小样本，不推断人声/距离/噪声识别率），`wake-acoustic-20261115-post-restart.json`。最终版同时记录播放期间真实麦克风 RMS/dB；此第二轮为短冒烟，不是第二次 7 分钟长测；其中另记录 1 个队列丢块并恢复，不能称零丢块。
- 首轮从音源开始到 USB 观察到唤醒约 2.077 / 1.955 / 1.858 s；WAV 本身约 2.114 s，这不是“说完后的响应延迟”，也不含完整 AI 对话。
- 首版自动化报告 `wake-acoustic-20261115.json` 的四段烧录通过但过早采到未就绪状态；未等待模型加载便报失败，不等于模型不能运行。该版报告的失败取证不完整，保留进程输出事实，不把其记录当合格验收。修订版已共享报告、阶段落盘和有界加载等待。
- 离线 **14/14 桌面 CTest、277/277 Python 主机回归**（含 14 项新自动化脚本测试）通过；软件桩测试不代表识别率。完整主机日志：`firmware/p4_gesture/test-results/wake-host-regression-20261115-final.log`。旧全量测试中 USB parser 桩缺少新增 binary callback 变量导致编译失败，已补全桩并输出编译 stderr，不修改生产 USB parser。

正式“猫猫”定制模型、真实人声/距离/噪声识别率、唤醒后 ASR/对话接入、实屏摄像头验收仍不是本轮完成项。

## 历史状态（2026-11-14）：持续推理失败，保护停用

- `cf27f6db…` 修订通过短采样与 90 秒监测后，仍在后续运行中发生 `stage=6` 推理崩溃；`failed-infer.elf` 与 `page-lifecycle.json` 保存现场。页切换测试的 **before 已是 panic 后新 boot**，不能归因于后续切页。
- 精确 PC `0x480b7cf8` 是 `esp.vst.128.ip q0,s8,0`，目标 `0x5010f810` 位于 LP RAM。安装的官方 IDF `components/heap/port/esp32p4/memory_layout.c` 明确说明 SPM/RTC RAM 不支持 128 位数据访问，且不带 `MALLOC_CAP_SIMD`。当前配置允许 RTC RAM 进入普通堆。
- 官方 P4 ESP-SR 2.6.2 `dl_lib_calloc` 反汇编调用普通 `calloc`，没有 SIMD 能力约束；因此模型重建/堆变化后可能返回不可用于向量指令的内存。这是本次 fault 的明确不兼容路径，修订效果仍须硬件验证。
- 本次只读检查：COM10 应用态可用，UI 帧数增长、麦克风运行；`PREVIOUS_WAKE_CRASH guarded=1 crash_stage=6`，唤醒已停用，未清除保护或继续要求用户说唤醒词。
- 正在验证局部 `dl_lib_calloc` / `dl_lib_calloc_psram` 链接包装：保留官方 free 前缀 ABI、零初始化和内部/外部优先策略，只增加 SIMD 能力与最低 16 字节对齐；不包装系统 malloc，不改官方权重、C5、音频和显示。归档 SHA 固定，升级必须重新审查。**未完成离线及长时间硬件验证前不能标记修复成功。**

### 2026-11-14 当前检查结果

- 实机枚举的是 **P4 应用态 COM10（VID/PID 303A:4005 composite）**，不是 ROM 下载态；当前没有看到 COM9。屏幕/UI 帧数继续增长、麦克风采集运行，未做复位或烧录。
- 设备仍运行旧 ELF `cf27f6db…`，唤醒状态为 `PREVIOUS_WAKE_CRASH / guarded=1 / crash_stage=6`，因此本次没有要求用户继续说唤醒词，也没有把旧状态误报为通过。
- 新版尚未烧录。离线产品增量构建成功：ELF SHA-256 `EE0541E823E91CCE00EE71931BF57BF9AFCDECBCDF29DF0DA904BD3C25DD7BAD`，14/14 桌面回归通过（含 SIMD 分配器、ESP-SR 适配和恢复测试）。下一步必须烧录后做长时间实机监测，至少覆盖模型就绪、持续推理、普通复位和页面/音频抑制；在此之前不宣称修复。

## 目标与词语边界

- 正式产品目标仍为 **猫猫**。本仓库所安装的 ESP-SR 2.6.2 没有该现成 WakeNet 资产；默认名 `wn9_maomao` 是待交付定制模型的配置名，不是已有官方模型。
- 用户已明确同意先用官方 **Hi，喵喵** 做真实声学验证。诊断包名为 `wn9_himiaomiao_tts`，模型元数据的精确词为 `Hi,喵喵`。
- 临时选择只写入被 Git 忽略的 `tools/config.toml`；提交的 Kconfig 默认值仍为猫猫。日志始终报告实际关键词和事件来源，不静默替换。
- 不把串口文本注入、推理函数返回、软件通过等同于用户真声唤醒、正确率或实屏验收。

## 实现链路

```text
唯一 I2S RX（48 kHz / S16 / stereo，保留左6/右5单端路由）
  ├─ 实时 RMS → AI 页麦克风图标和波形（暂停时不冒充正在采集）
  └─ 选已校准声道 → 零等待有界队列（12×10 ms）
       → 独立 wake_sr 任务
       → 63-tap FIR / 3 倍抽取（16 kHz mono，跨块保留相位）
       → 精确 WakeNet chunk → 真实识别事件
       → 琥珀色监听提示 + 短暂好奇表情
```

保护规则：
- 模型加载与队列创建在 GUI 预热后由 app service 发起，不占用 LVGL 锁；推理不在麦克风采集回调运行。
- 只有 AI 首页、实际采集且非语音播放/演示时请求唤醒。功放活动和音频自检即时抑制，并保留 300 ms 静音尾窗。
- 切页、停止、播放和重新启用通过 epoch 隔离；队列丢块触发状态重置，超过 200 ms 的旧音频不推理，推理中被取消的结果不提交。
- 连续识别有 1.8 s 冷却，防止一次话语重复触发。
- 在官方模型解析器前校验包表、计数、字符串、偏移、长度和重叠；空包/擦除状态/错误词/错误采样率或声道均 fail-closed。
- 结构校验不是模型签名或真实性认证；定制模型仍应来自可信来源。

## 本机配置与构建

```toml
[features]
espsr = true

[espsr]
# 仅用于这次获同意的官方模型诊断；不代表猫猫已实现
diagnostic_preset = "hi_miaomiao"
```

正式定制资产到位后，清空诊断 preset，改为：

```toml
[espsr]
diagnostic_preset = ""
model_dir = "本机资产根目录"
model_name = "wn9_maomao"
```

根目录只能包含选定模型目录，内部为 `_MODEL_INFO_`、`wn9_data` 和 `wn9_index`。工具验证元数据的精确词为猫猫，并计算 SHA；改资产会使生成包失效，不复用旧词包。路径、模型权重和产物不入 Git。

```powershell
python tools/meoave.py build --profile product
python tools/meoave.py hardware --cycles 1 --restarts 0 --report test-results/espsr-wake-20261114/新烧录报告.json
python tools/wake_acceptance.py --listen-seconds 60 --min-detections 1 --report firmware/p4_gesture/test-results/espsr-wake-20261114/新声学报告.json
```

只增加原 16 MB flash 尾部的 `model`：0xF10000 / 960 KiB。原 factory、NVS 和 7 MiB storage 地址/容量不变；超出保留容量的定制模型必须另行设计存储，不自动覆盖用户存储。

## 可观测与验收

`UI` 增加 `MEO:WAKE_STATUS`：
- linked / loaded / keyword_ready / requested / armed / blocked；
- 实际 model / keyword / error；
- queued / drops / chunks；
- acoustic / text_tests / source。

`MEO:WAKE` 的 source：
- `ACOUSTIC_ESP_SR`：真实麦克风推理事件；
- `TEXT_BRIDGE_TEST`：显式串口测试，单独计数，不改写引擎 backend。

测试覆盖：
- CTest：现有 UI、模型、音频、对话回归，以及真实 adapter 代码配模型/RTOS 桩的排队、声道、失效代次、溢出、播放抑制、旧结果和冷却测试。
- DSP：任意分块不变性、抽取样本数、通/阻带、清零；模型包 10000 次损坏输入安全检查。
- Python：资产词匹配、诊断 preset、切回正式词、空/错模型包、构建签名和既有链接/ABI 约束。
- 桩测试不证明声学识别率。硬件监视工具不注入文字、不放测试音、不复位、不动 C5；无识别事件时只报告推理链路，不标记真声验收完成。

证据目录：`firmware/p4_gesture/test-results/espsr-wake-20261114`。
首个 `flash.json` 是空猫猫模型的 fail-closed 基础固件，不是 Hi，喵喵 识别验收；诊断模型的烧录与监听使用后续独立报告，保留旧证据。

## 仍需完成

- 实机人声多次唤醒、安静误唤醒观察、不同距离/噪声和边播边听策略。
- 用户目视确认麦克风提示、波形、唤醒表情和显示方向。
- 真正的猫猫定制声学模型及其独立验收。
- 唤醒后的真实采集上传/ASR 对话轮接入；本轮不自动启动固定计时假对话充当回答。

不改变显示初始化、触摸引脚、180° 单次旋转、PWR_HOLD、原生 USB、48 kHz codec 时钟、GPIO6 音频、GPIO54 雷达 SOP0，也不更新 C5 固件。

### 实机迭代记录（保留失败证据）

- 第一版官方 Hi，喵喵 的四段烧录和自动启动通过，但只读运行检查返回 `QUEUE_ALLOC_FAILED`：内部 SRAM 不足，模型未就绪、armed=0。报告 `flash-hi-miaomiao.json` 和 `pipeline-hi-miaomiao.json` 分别记录烧录成功与功能失败，不合并为唤醒通过。
- 修订将队列 payload 和独立推理任务栈显式放到 PSRAM，RTOS 控制块保留内部静态存储；使用本板已经启用的外部静态栈能力，不改 SDK 配置、显示 DMA 或音频路由。静态任务在模型失败时永久挂起，分配/创建失败有回收测试。
- 修订仅增量编译 9 个步骤，非全量编译；后续烧录和实际推理进度需以新报告为准。
- PSRAM 修订的 `flash-hi-miaomiao-psram.json` 已确认四段镜像写入、Hash 校验、CPU-only 启动、运行 ELF 身份、PWR_HOLD 和触摸通信通过；运行 ELF SHA 为 `177f3d9854e9aff3d70ce1790a287d6a62bcd653ac6cea2b774c488f8ea4d7f7`。烧录报告采样仍处于启动预热，不证明模型已经加载。
- 随后的 `pipeline-hi-miaomiao-psram.json` 未能取得任何推理状态：应用 USB 不存在，Windows 独立枚举也未发现原生 USB 或串口。尚不能区分连接中断与设备运行故障，不能将其归因为模型、也不能将 PSRAM 修订记为实机修复成功。已请求用户确认画面并重新插接原生 USB；未追加盲目复位或重刷，未操作 C5。
- 本次修订后的完整桌面 CTest 为 **12/12** 通过（`ctest-post-flash.log`）；Python 模型工具 **11/11**、既有链接/构建检查 **13/13**、唤醒验收工具 **4/4** 通过。均为软件回归，不替代真声唤醒或实屏确认。

### 持续重启追查与下一版保护（2026-11-14）

- 用户确认 PSRAM 栈版本持续重启。重新连接后采到旧 ELF 的 `SYS:FAULT valid=1 core=0 pc=48107a5e ra=4804bd06 sp=4ffb5840 cause=5 bad=3f800000`，并采到 `reset=4`。旧 ELF 已留作本地取证；`addr2line` 对应 `ppa_do_scale_rotate_mirror()` / `lvgl_port_ppa_rotate()`，是无效 PPA 客户端句柄的 load access fault，不是 USB 线缆结论。
- 这只能定位到崩溃点，尚不能证明是 WakeNet 本体、外部栈或哪次分配直接破坏句柄。新增内部 BSS 挤压显示/PPA 内存是待验证假设；不将假设写作已证实根因。
- 已通过一次原生 USB 下载命令停留 ROM，未操作 C5。第一轮修订烧录在打开 COM9 时失败，**没有写入任何镜像**；设备随后又枚举为旧应用，后续尝试使用全新报告并重新走应用→ROM 链路，不复用失效端口。
- 修订把 4 KiB PCM scratch 从内部 BSS 移到 16 字节对齐的 PSRAM；队列 payload 仍在 PSRAM。8 KiB 推理任务栈改为显式内部 SRAM，避免供应商代码进入 cache-disabled 路径时使用外部栈；分配不足只报告错误，不重启。
- 增加按 ELF 身份隔离的 RTC 故障阶段记录：1=包校验、2=映射、3=创建、4=格式/元数据、5=清状态、6=推理、7=释放。若在这些调用期间 panic/WDT，下一次同版本暖启动禁用唤醒并报告 `PREVIOUS_WAKE_CRASH` / `guarded=1`；新 ELF 或冷上电才给新机会，不自动重试坏模型。该保护只覆盖唤醒调用，不宣称能拦截此前发生的显示初始化崩溃。
- `MEO:WAKE_STATUS` 增加 `guarded`、`crash_stage`、`stack_free`。离线测试覆盖内部/外部各次分配失败回收、阶段记录、panic/WDT、同版本软件重启继续降级、新 ELF、冷启动和损坏 RTC 记录。
- 验收监视器保留断连前的部分采样，要求最终 `requested=1 armed=1`，拒绝模型就绪丢失/降级/意外复位；最低人声次数未达标也先记录启动身份和故障。完整桌面 **12/12**、监视器 **7/7** 回归通过；硬件结果另用新的报告记录，不能据此宣称修复完成。

### 已证实的显示内存失败与恢复链路修订

- `flash-guard-pcm-02.json` 的四段 Hash 通过，但**运行验收失败**，显示停在 `ready=0 stage=3`。同一次启动日志明确记录 `ppa_core: no mem to register PPA SRM engine`、`unable to acquire SRM engine` 和 `lvgl_port_ppa_create: Error when registering PPA client`；这不是模型就绪或显示成功。该次故障发生在唤醒模型初始化之前。
- 本地安装的 `esp_lvgl_port 2.7.2` 中，PPA 创建失败后释放 context，却返回该已释放指针；context 自身分配失败时还会解引用空指针。它解释了此分配错误为何会继续进入无效 PPA 句柄使用路径。保留旧 panic 与新分配日志，不能把后续 `valid=0` 解释为此前没有故障。
- 新版将产品 Hosted/Wi-Fi 初始化延后到 UI 预热完成，避免其 10 KiB/8 KiB worker 栈及其他内部内存分配抢在显示/PPA 之前；临时 16 KiB 显示创建任务退出后再运行后续服务。非 UI/维护配置保留原行为，并增加显示前及 Wi-Fi 启动前的内部空闲量/最大块日志。
- `cmake/PpaGuard.cmake` 只在构建目录生成上游源码的错误处理修正版，不修改 `managed_components`。对原文件的换行归一化 SHA 做严格校验，升级不匹配就停止构建要求审查；成功路径和全部旋转计算保持原样。修正 NULL 检查、分配失败释放与失败返回 NULL；**上层仍可能因无法创建显示而断言，此补丁本身不承诺低内存时能显示或不会重启**。
- 旧固件在显示卡住后，CDC 下载请求无响应；进一步使用有 nonce 的 `SYS RESTART` 得到 `accepted=0`。旧请求路径必须临时创建 4 KiB reset 任务，内存耗尽时无法创建。新版改为启动时静态预留内部栈/TCB和等待通知的恢复任务，请求路径不再申请堆内存；保留原有供电检查、USB断开、下载标志及复位时序。
- 新增 `tests/test_recovery_guard.py`：实际生成的 PPA factory 经 context/buffer/engine/callback 失败注入及成功路径校验；原始源码不变、构建副本幂等、升级拒绝；实际 USB 请求/恢复函数覆盖重复拒绝、下载/普通复位、供电不满足拒绝和未初始化拒绝。**4/4** 通过，并纳入完整 CTest **13/13** 通过（`ctest-recovery.log`）。监视器 **7/7** 通过。
- 修订版产品增量构建通过，ELF SHA `c947e11729ba00126d547003154271692e3bcbb7150e3d096d8117eecb0f82a8`，APP 大小 `0x287e40`；日志 `build-recovery.log`。`flash-priority.json` 下载切换超时，`flash-recovery.json` 无目标 USB，均未写入修订版；后续使用独立报告 `flash-recovery-03.json`，不可用编译或桌面测试结果替代烧录/运行结果。

### 第二个独立故障：WakeNet 状态清理

- `flash-recovery-03.json` 已通过四段 Hash、CPU-only 自动启动、运行 SHA/供电及显示/触摸进度检查。但检查窗口之后发生一次新的 panic；`pipeline-recovery.json` 记录 `reset=4`、`guarded=1 crash_stage=5`、`PREVIOUS_WAKE_CRASH`，**该版仍未通过唤醒验收**。保护使其不再重复进入有故障的唤醒调用，随后 `ready=1 stage=7`、触摸及 PONG 持续工作。实屏仍需用户确认。
- 新 panic 的 PC `0x480b2e16` / RA `0x480af96a` / bad address `0x10`，用保留的 `failed-clean.elf` 解码到 `dl_convq_queue_bzero()` / `model_clean()`。当前 P4 预编译 WakeNet9 实现的工厂为 queue 指针表分配 24 字节（6 项），而 clean 遍历 36 字节（9 项）；这是独立于此前显示 PPA 的错误路径。没有通过修改私有模型结构或绕过取消检查来掩盖它。
- 同时发现官方 `wn9_himiaomiao_tts` 资产元数据实际为 `wakenet9l_tts1h8_Hi,喵喵_3_0.636_0.641`，而按目录前缀选型会得到通用 9 引擎。修订从实际元数据严格选择 `wn9_` / `wn9l_` / `wn9s_` 官方接口；未知类型 fail-closed。原模型目录、权重、包名和关键词都不改名，工厂仍接收原始资产名。
- 对已确认有问题的 clean 路径，修订采用公开 `destroy/create` 生命周期重建来隔离不连续音频；初始新模型无需重复清理。仅在有真实推理状态后重建，重建期间停收，失败则禁用，仍保留 epoch/过期音频/播放尾窗和故障阶段保护。不重新映射模型分区，也不把旧状态带到新话语。实际耗时、掉块与持续运行需要后续硬件报告验证。
- 接受事件在推理结束时已做严格检查；主线程不能因随后的短暂模型重建丢掉已接受事件，离开 AI 页仍消费并丢弃，不回放旧事件。监视器只容许明确的 `MODEL_REBUILDING` 短暂状态（最多 3 秒），结尾必须重新就绪、armed=1，永久丢失、故障、复位或文本注入依旧失败。
- 扩展桩回归覆盖元数据与目录别名不一致、未知架构、首次不调用 clean、脏状态重建、创建失败停收和事件不丢失。完整 CTest **13/13**（`ctest-lifecycle.log`）、监视器 **8/8** 通过；产品仅增量 **10 步**，APP `0x288290`。新烧录使用 `flash-lifecycle.json`，未完成真实采样前不标记功能成功。
