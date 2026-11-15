# Meoave 句子级语音与表情系统

更新：2026-11-13。以本文件覆盖旧的「仅七种／仅状态表情／首页禁止说话嘴型」方案。

## 1. 最新硬目标

- **每个回复句子都有非空表情链**，不仅是 LISTENING / THINKING / SPEAKING 状态。
- 每个 cue 包含表情、0..1 强度、相对音频时间、过渡时间；至少 16 种真正不同的动画。
- P4 根据已完成输出的 PCM 帧推进；网络到包、TTS 完成、I2S write 返回都不能充当播放时钟。
- 暂停／欠载不空跑，取消关闭 PA、清理队列并封锁旧 generation；旧音频、旧表情不能复活。
- 小嘴巴与实际 PCM 包络联动；静音／停播闭嘴。待机仍然只有黑白眼睛，无电量、Wi-Fi、状态文字。
- 「设置 → 表情预览」可切换 16 种、轮播、开关嘴巴，比较 30% / 60% / 100% 强度。
- IndexTTS2 或更低延迟引擎在 PC／GPU 网关运行，P4 只接收音频和有界表情数据。

## 2. 已落地的代码边界

- `meoave_model.c`：保留前七个枚举 ID，追加到 16 种；统一眼形／嘴形和强度、独立微动作。
- 16 种：平静、开心、好奇、困倦、惊讶、眨眼、难过、兴奋、害羞、喜爱、得意、疑惑、担心、生气、专注、安心。
- `meoave_sentence.c`：无平台依赖的校验、代际隔离、顺序句子、音频帧时间线、平滑过渡；跳帧直接求当前姿态，不补播过时表情。
- `meoave_sentence_json.c`：设备真实 JSON 解析，固定上界、深度限制、拒绝重复字段／非法浮点／空链／错误音频格式。
- `meoave_pcm.c`：12 块内部 SRAM 环形队列；每块 240 帧，六个现有 DMA 描述符。ISR 只操作固定 PCM／整数状态，不做浮点、分配、日志、LVGL 或网络。
- `bsp_audio.c`：在已有 I2S 启用前注册 on_sent；仅在持有语音输出租约时接管已完成描述符。统计旧载荷完成，再填下一圈；欠载填零、不推进语音帧。
- 时钟精度边界：DMA 完成前沿以 240/48000 = 5ms 为粒度，仍有硬件 FIFO/DAC 与屏幕刷新延迟；不是纳秒级声学同步，当前未校准扬声器到屏幕的物理延迟。
- `meoave_voice.c`：真实播放 worker，48k/S16/mono 输入复制为已验证的双声道输出；校验 manifest 与 PCM 长度、输出租约、完成排空、取消和无进度超时。
- `meoave_ui.c`：预览与首页共享 renderer，独立模型／缓存；说话才出现小嘴巴。减少动态效果时 cue 仍切换，取消连续微动作／口型形变。
- 发现并修复像素量化缺陷：眨眼／形变过程中全部路径点重合会被 LVGL 跳过，导致一只眼瞬间消失；现在保留最小 1px 圆角脊线。

**尚未完成**：连续录音／VAD／ASR／真实 LLM、IndexTTS2 权重部署与本机测速、AEC／自然打断。生产播放链路只接受服务器生成并校验过的句子元数据和 PCM 流。

## 3. 一句一包协议 v1

```json
{
  "v": 1,
  "generation": 1,
  "sentence_id": 1,
  "sample_rate": 48000,
  "channels": 1,
  "format": "pcm_s16le",
  "frames": 96000,
  "text": "你好，我在这里。",
  "expressions": [
    {"at_ms": 0, "expression": "CURIOUS", "intensity": 0.5, "transition_ms": 160},
    {"at_ms": 700, "expression": "HAPPY", "intensity": 0.8, "transition_ms": 180},
    {"at_ms": 1400, "expression": "RELIEVED", "intensity": 0.65, "transition_ms": 200}
  ]
}
```

- 30 秒／句、768 UTF-8 字节文本、32 cue、8192 字节 JSON、最多六层嵌套。PCM 帧数是采样点数，不是字节／立体声样本数。
- 首 cue 必须为 0，后续严格递增、落在音频内；0..2000ms 过渡不能跨下一个 cue／句末。强度必须有限且 0..1。
- generation 每轮严格增加；sentence_id 从 1 连续递增。重复、旧轮、乱序、并行起播拒绝。取消封锁本轮，下一次必须新 generation。
- 每句缺链时**拒绝**，不悄悄丢掉表情，也不将整个段落配一个 HAPPY。网关示例先校验每句，再调用 TTS。
- 音频文件与 manifest 先生成完整再发布，保持显式一致性。未来二进制传输仍须补 session／generation／sentence／seq、长度／哈希及认证，现有文本 USB 不是音频传输协议。
- 控制白名单仅为有限表情；不能传绘图脚本、GPIO、复位、升级或 PC 命令。

## 4. 网关与 TTS

`tools/voice_gateway.py` 保留为历史句子编译器／IndexTTS2 研究适配器，不是生产入口。当前唯一运行时服务端是 `server/` 下的 Go 实现；它不依赖 Python、FFmpeg 或系统级 Go，直接调用 Edge-TTS Go 客户端并把音频编译成设备协议。

```powershell
$go = "C:\Users\ijink\Desktop\Projects\meoave\tools\.venv\go-runtime\go\bin\go.exe"
& $go -C server test ./...
& $go -C server run ./cmd/meoave-server
```

Go 服务端入口：

```text
GET  /healthz
POST /v1/synthesize
POST /v1/cancel
```

Python 命令只用于历史 IndexTTS2 研究，不得作为设备运行链路或部署步骤。

```powershell
python tools/voice_gateway.py validate --plan tools/voice_plan.example.json
# 下条在单独的 upstream IndexTTS Python 环境运行，不在 ESP-IDF venv：
python tools/voice_gateway.py synthesize --plan tools/voice_plan.example.json --config tools/config.toml --out test-results/tts-new-run
```

- 模型／参考音色／本机目录在 Git 忽略的 `tools/config.toml` 的 `[voice]` 中；模板在 `tools/config.example.toml`。不得将个人克隆音色或密钥提交 Git。
- 要求固定审查过的 upstream Git SHA、显式本地辅助权重；关闭默认 Qwen 文本情绪分析模型，用 LLM 已给出的情绪链转换八轴 voice emotion，节省不必要的模型加载。
- 人脸 16 类不等于 TTS 八维一一对应。适配器按时长加权一个句子级声音情绪；**不宣称 TTS 可在每个 cue／音素内切换声音情绪**。
- 先生成每句 WAV，正规低通重采样至 48k 单声道，输出 `.pcm` 和设备 JSON；报 synthesis_ms、音频长度、RTF、权重加载时间。不伪造首包延迟。
- 示例 LLM 链使用 `at:0..1` 的语义相对位置，再映射合成后的真实总帧数。标注 `proportional_not_word_aligned`：这是句内近似落点，**不是字级强制对齐**。需要字级同步时再接 forced alignment。
- 当前按整句准备音频，保证时轴完整；不是子句内边生成边播。下一句可以后续预生成，但当前尚未实现并发预取／WSS 下发。
- 只使用自有／获许可音色，部署前核对官方代码及模型许可，不默认所有权重都可任意商业化。

### 选型结论（官方资料核对于 2026-11-13）

1. **IndexTTS2**：优先保留情绪与音色控制；当前官方 `infer_v2` 的 streaming 也是内部文本段生成完才 yield，不能把它当逐音频 token 低首包接口。
2. **CosyVoice3**：可作为更低首包的流式候选。官方低延迟数字属于上游测量，不能直接套用本机，更不能和另一块 GPU 上的 IndexTTS 数字混比。
3. 官方同仓库目前另有 **IndexTTS 2.5** 与加速宣传，但不是用户指定的 2.0；若尝试，应单独适配、同机测量后选，不静默换版本。
4. 本机实测 GPU 为 RTX 5070 Laptop、8151MiB。尚未部署权重／确认可用内存峰值，未声称某引擎已跑通或更快。
5. 用户最新选择：**先完成设备端同步**，暂缓 GPU TTS 部署，不下载模型。下述本地 Windows SAPI 音频仅用于验证设备播放与表情时轴。

官方依据：
- https://github.com/index-tts/index-tts （README、LICENSE、MODEL_LICENSE）
- https://github.com/index-tts/index-tts/blob/main/indextts/infer_v2.py （IndexTTS2、情绪顺序、offline aux_paths）
- https://github.com/FunAudioLLM/CosyVoice （双流式与官方测量范围）
- 本机锁定 ESP-IDF v6.1 `esp_driver_i2s/i2s_common.c` 的 on_sent 与 DMA cache writeback 实现。

## 5. 生产链路与分层验收

生产语音不再内置演示音频，也不提供固定回复状态机。服务器通过 `VOICE OPEN`
建立会话，再发送经过句子协议校验的 META、PCM 和 END 帧；P4 按实际 DMA
完成帧驱动嘴巴和表情链。`AI STATUS` 只报告真实语音状态，`AI CANCEL` 只取消
当前生产会话。

测试只针对当前改动，不全跑历史 C5／黑屏／音频回环长压测：

```powershell
cmake --build firmware/p4_gesture/tests/desktop/build -j 8
ctest --test-dir firmware/p4_gesture/tests/desktop/build -R 'ui\.|model.state|audio.adapter|conversation\.sentence|conversation\.voice_worker' --output-on-failure -j 2
python -m unittest discover -s firmware/p4_gesture/tests -p test_voice_gateway.py -v
& $go -C server test ./...
python tools/meoave.py build --profile product
python tools/meoave.py hardware --cycles 1 --restarts 0 --report <本轮新报告绝对路径>
```

门槛按层分别记录：
1. 纯 C／解析：16 种姿态与动画不同、强度 0 归中、过渡、缺链拒绝、畸形／深度／随机输入、暂停／欠载／取消／旧轮隔离。
2. 生产 worker：真实代码＋模拟 DMA，排空／完成、长度、任务创建失败、取消及无进度超时；不等于物理扬声器。
3. 同套 LVGL：16 种、强度、嘴型、触摸、首页无状态条；桌面不是实屏。
4. 板上：新 ELF 身份、同一次启动、生产服务器下发的句子完整播放帧数、表情 cue、零故障，音量遵循用户设置；还需用户听感与实屏确认。
5. 真实 AI：P4 录音→ASR／LLM→GPU TTS→WSS→P4 播放及表情；**尚未完成**。

## 6. 不动的硬边界与既有证据

- 原生 USB 动态识别，不用 COM3。此次只更新 P4，不刷写／主动复位 C5，不碰下载三脚。
- 显示初始化、触摸引脚、电源保持、单次 180° 旋转、48k 音频时钟、单端麦克风左 6／右 5、GPIO6 音频和 GPIO54 雷达 SOP0 不变。
- 存在普通程序复位后 USB 失联／WDT 的历史未决问题；本轮 `--restarts 0`，不借烧录后的首次启动宣称普通复位已修好。
- 上轮七表情烧录报告：`test-results/expression-preview-20261113/usb-acceptance-02.json`；它只证明旧版烧录与启动，不是本轮 16 表情或真实 TTS 验收。
- 本轮证据统一放 `test-results/sentence-expressions-20261113`，先保留红测试和首轮编译失败，再记录修订后的绿测试与板上结果。

## 7. 本轮实际结果与未决项（2026-11-13）

### 软件与烧录

- 相关七项 CTest、四项网关测试通过；同套 LVGL 生成 16 表情及强度拼图。不是实屏照片，不替代逐项触摸与视觉验收。
- 首轮镜像写入校验通过，但启动时 PPA 内部 SRAM 分配失败并 panic；失败报告 `usb-acceptance-01.json` 保留。语音 ring 改为播放时按需分配内部 SRAM，释放后不占启动峰值；BSS 从 53040B 降至 41400B。
- 添加 cJSON 时曾触发间接 USB 依赖升级，已显式锁回原验证的 TinyUSB 0.21.0~4／USB 1.6.0；不借本轮变更升级下载链路。
- 修订版 `usb-acceptance-02.json`：三段写入 Hash、受控自动启动、运行身份、供电保持与触摸通信通过。ELF SHA 为 `969bbc4978d7cb24e5cfc689625bf22b44f8e71f461d2f22f10f1b55997607c8`。`program_restarts=[]`，不代表额外普通程序复位通过。
- `voice-acceptance-01.json`：30% 音量下，两句 DMA 完成、六 cue、重复拒绝、取消及同一 boot 的软件判据通过；**用户随后反馈没有语音声音，因此该轮物理听感未通过**。不能用串口结果覆盖用户反馈。

### 无声追查与用户验收

- `no-speech-diagnostic-01.json`：只读原生 USB 检查；原设置音量 100%，codec 身份／寄存器／静音状态正常。没有复位、刷写或修改 C5。
- 历史本地语音样例不再作为当前产品验收依据；生产验收必须使用服务器生成的句子和真实 VOICE 流。
- `physical-acceptance-followup-01.json`：最终只读检查仍为原 boot，回到 `eyes`、`eye_auto=1`、语音 idle/error=0、音量恢复 100%、FAULT valid=0。用户反馈与串口证据分开记录。
- 首轮无声根因尚未确定：30% 到 50% 的对比及先播放音乐不是单变量因果实验，不能断言仅是音量问题，也不能宣称彻底修复音频链路。

### 尚不算完成的部分

- 16 种表情和各档强度尚未逐项实屏验收；物理声画延迟未测量／校准。
- 试听音乐本轮未听到；不将驱动返回成功算听感成功。
- 不是完整 AI 对话：未接 GPU TTS、WSS、录音／ASR／LLM；保持用户「先设备端同步」的范围。

### 2026-11-14 音乐问题续查

用户后续在两次 100% 重播中确认「音乐能听到，但很小」，更新了前述无声现象。查到旧试听素材峰值被固定为 7000，已修订至 22000 并验证采样余量、实际 renderer 电平及来源完整性；不改语音、全局音量曲线或硬件增益。增强版烧录与 60%／静音／100% 软件验收通过；用户确认显示正常、音乐变大但仍偏小，随后要求停止修改，保留当前增强版及原 100% 音量。响度不标记通过。详情以 `docs/AUDIO_LOUDNESS_20261114.md` 为准，不能据素材修订宣称首轮语音无声原因已查明。

### 2026-11-14 云端 TTS 候选实测（未接入设备）

- 本机没有 Azure Speech 密钥／区域。用户明确同意改测 Edge-TTS；不把 Edge 的结果冒充 Azure 正式 API，也不据此取消或完成 IndexTTS2 的待办。
- 独立 Python 环境的 edge-tts 7.3.1 测试是历史基准；生产候选已切换为 Go 直接调用 `github.com/leopold7/edge-tts-go@v0.1.6`，默认使用 zh-CN-XiaoxiaoNeural。Go 服务端真实冒烟已收到有效的 24 kHz MP3，并完成 48 kHz 单声道 PCM 输出。
- 首音频数据中位数：短句 1250 ms、中句 1406 ms、长句 1245 ms；stream 完成中位数：1585／1881／1778 ms。音频本身时长分别约 2.112／5.112／11.808 秒。首音频整体范围 1184..1495 ms，stream 完成范围 1531..1921 ms。
- 时间包含服务端建立请求及接收音频，不含录音、ASR、大模型、P4 网络传输和实际出声；Go 端另行记录 MP3 解码／48 kHz 重采样耗时，没有设备烧录、播放或声画同步验收。当前设备整句协议仍需完整音频及帧数，不能把首包时间当成现有设备首声。
- 每种仅三次，不报告 P95，不宣称长期稳定性或服务可用性保证。Go 服务端已经接入 Edge-TTS 候选，但尚未接入 P4 真实传输。
- 可重复工具：`tools/benchmark_tts.py`，可选依赖 `tools/requirements-tts-bench.txt`；证据和试听 MP3 在本地忽略的 `test-results/edge-tts-latency-20261114`，不提交音频或运行环境。

### 2026-11-14 AI 页面监听提示与「猫猫」唤醒词

- 历史版本曾在 I2S RX 工作时常驻显示麦克风和波形；2026-11-15 按用户最新要求改为：AI 页保持待命，但只有新鲜且持续的说话输入电平才显示右下角 32px 麦克风，胶囊内部随实时 RMS 填充，静音隐藏。自身播放及其 300ms 尾窗不显示；不遮挡表情、不变更采集／ESP-SR。实现边界与本轮验收见 `docs/LISTENING_INDICATOR_20261115.md`；电平门限不是 ASR/VAD 人声分类。
- P4 的可观测状态增加 `MEO:UI_AUDIO mic_running=1 wake_armed=1`，用于区分「页面看起来像在听」和「硬件确实已启动采集」。本轮实机报告确认这两个状态均为 1。
- 唤醒词统一为 `猫猫`，公共接口在 `meoave_wake.[ch]`；唤醒后波形/环颜色短暂变为琥珀色，事件通过 `MEO:WAKE ... detected=1` 输出。
- 本轮已修复 USB CDC 对 UTF-8 字节的过滤以及 `AI WAKE ` 前缀长度错误，`AI WAKE 猫猫` 已在 COM10 上实际触发并通过 `TEXT_BRIDGE` 记录。
- **边界必须保留**：当前 `TEXT_BRIDGE` 只验证「上游 ASR／服务器提交识别文本 → P4 唤醒反馈」这条链路，并不等于 P4 已经能直接从麦克风声学识别中文“猫猫”。真正离线声学唤醒仍需接入中文模型（ESP-SR／自定义模型）或完成 P4→Go 服务端 ASR；不能把文本桥测试写成真实声学唤醒已完成。
- 烧录证据：`firmware/p4_gesture/test-results/ai_mic_wake_final_flash_20261114.json`。P4 三段镜像 Hash 校验、CPU-only reset、运行 ELF SHA、PWR_HOLD 和 USB 应用态均通过；C5 未刷写。

### 2026-11-14 ESP-SR 实施更新

新增的声学实现与验收边界以 `docs/ESP_SR_WAKE.md` 为准。ESP-SR 2.6.2 已实际链接，增加独立推理任务、48→16 kHz FIR、有界队列、播放抑制与模型安全校验。旧的 TEXT_BRIDGE 仅是历史文字联调，不是声学成功；新版将引擎 backend 与 event source 分开，并区分 requested 和 armed。用户已同意先使用官方 Hi，喵喵 模型实测，正式猫猫目标未完成。无有效模型时 armed=0，不再把采集运行误写为关键词就绪。
