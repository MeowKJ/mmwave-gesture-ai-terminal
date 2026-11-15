# Meoave Go Server

## 当前电脑本地部署（2026-11-15）

唯一运行时仍是 Go，不需要 Python、FFmpeg 或本地大模型。
电脑负责 HTTP 会话、云端模型调用、Edge-TTS、解码及原生 USB 下发；
这不是离线 LLM/TTS，也尚未接入麦克风 ASR。

1. 复制 `server/config.example.json` 为 `server/config.local.json`，填写本机配置。
   后者已被 Git 忽略。API key 只放该文件或 `MEOAVE_LLM_API_KEY` 环境变量，不写入脚本。
2. 从仓库根目录运行：

```powershell
./tools/server.ps1 Start
./tools/server.ps1 Status
./tools/server.ps1 Test
./tools/server.ps1 Stop
```

`Start` 增量编译并后台运行；只监听 `127.0.0.1:8787`，不开放公网或改防火墙。
自动端口识别仅接受 P4 应用态原生 USB VID/PID `303A:4005`，不回退 COM3、ROM 下载或硬件复位。
配置变化需要 `Stop` 后重新 `Start`。不连设备时 `Start -ServerOnly`；
只测云端模型和 TTS 时 `Test -ServerOnly`。后者不能当作设备播放成功。

当前模板使用 DeepSeek 官方端点、`deepseek-flash`、非思考 `none` 和 JSON 输出。
如果使用第三方服务，必须按该服务的实际 API 地址、模型 ID 和参数配置，不能只替换密钥。
`llm_configured` 仅表示配置存在，不代表账号鉴权或余额检查通过。
环境变量里的 API key 优先于本机文件；启动日志和进程参数不携带 key。

`Test` 使用真正的 HTTP `/v1/chat`，每次只发一轮，不自动重试；
报告保存在 `test-results/local-server/acceptance-*.json`，失败返回非零退出码。
报告检查每句表情链、实际 PCM SHA/帧数和设备的完整 DMA 播放回执；不写入整段 Base64 或密钥。
离线测试的替身、设备遥测与用户听感／实屏验收必须分开。

```http
POST /v1/chat
Content-Type: application/json

{"generation":1794744000,"text":"猫猫，今天有点累。","play":true}
```

generation 必须比该服务当前值更新；每轮包含一到两句，句子 ID 从 1 递增，
共享同一 generation。所有句子均完成表情链和音频时轴校验后才开始物理播放；
取消上下文覆盖整个合成／播放过程，不在合成结束时提前丢弃。
`play:false` 只生成语音，不让设备出声。响应明确标注 `input:text`、`asr_tested:false`。
本轮实际结果与尚未完成项目见 `docs/LOCAL_SERVER_20261115.md`；以下旧版 CLI 示例保留为历史接口说明。

这是 Meoave 唯一的服务端实现。它是一个模块化单体，不需要 Python 或 FFmpeg 运行：

1. Go 调用 Edge-TTS。
2. Go 解码 MP3。
3. Go 转单声道并重采样为 48 kHz S16LE。
4. Go 校验每句表情链并输出设备协议。
5. 可选地通过 P4 原生 USB 的 CDC 端口（例如 COM10，不使用 COM3）把句子元数据和 PCM 流送到设备。

另外提供 `meoave-ai-test`：从环境变量读取 OpenAI-compatible Gemini 配置，验证模型返回的句子/表情 JSON，再逐句走同一套 Go TTS、解码和 PCM 时轴校验。

## 启动

```powershell
$go = "C:\Users\ijink\Desktop\Projects\meoave\tools\.venv\go-runtime\go\bin\go.exe"
& $go -C server run ./cmd/meoave-server
```

默认地址：`http://127.0.0.1:8787`。可用环境变量或参数覆盖：

```powershell
$env:MEOAVE_TTS_VOICE = "zh-CN-XiaoxiaoNeural"
& $go -C server run ./cmd/meoave-server -listen 127.0.0.1:8787
```

启用硬件联调时，把 P4 当前应用态的原生 USB CDC 端口传给 Go 服务端：

```powershell
$env:MEOAVE_DEVICE_PORT = "COM10"
$env:MEOAVE_DEVICE_BAUD = "460800"
& $go -C server run ./cmd/meoave-server
```

服务端提供 `POST /v1/play`。请求体与 `/v1/synthesize` 相同；它会先走同一套 Edge-TTS、PCM 校验和表情链校验，再通过 CDC 二进制帧播放。设备端协议不是把 PCM 塞进文本命令，而是 `VOICE OPEN` 后使用带 generation、sentence_id、seq、长度和 CRC16 的帧；P4 边收边播，避免整句音频占满内存。

示例：

```powershell
$body = @{
  generation = 1
  sentence_id = 1
  text = "你好，我在这里。"
  expressions = @(
    @{ at_ms = 0; expression = "CURIOUS"; intensity = 0.5; transition_ms = 160 },
    @{ at_ms = 700; expression = "RELIEVED"; intensity = 0.65; transition_ms = 200 }
  )
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8787/v1/play -ContentType "application/json" -Body $body
```

`/v1/play` 只在配置 `MEOAVE_DEVICE_PORT` 时启用；没有设备时返回 503，不会假装播放成功。

## 本机请求

```powershell
$body = @{
  generation = 1
  sentence_id = 1
  text = "你好，我在这里。"
  expressions = @(
    @{ at_ms = 0; expression = "CURIOUS"; intensity = 0.5; transition_ms = 160 },
    @{ at_ms = 700; expression = "RELIEVED"; intensity = 0.65; transition_ms = 200 }
  )
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8787/v1/synthesize -ContentType "application/json" -Body $body
```

`generation` 必须严格递增；新 generation 会取消旧请求。每句必须有至少一个 cue，首 cue 必须从 `at_ms=0` 开始，且只允许固件已有的 16 种表情。

## 测试

```powershell
& $go -C server test ./...
```

离线测试不访问网络。启动服务后再发一次请求，才是当前电脑的真实 Edge-TTS 测试。

## Gemini 自动测试

不要把 API key 写进仓库或命令历史，使用临时环境变量：

```powershell
$env:MEOAVE_LLM_API_KEY = "<your-key>"
$env:MEOAVE_LLM_BASE_URL = "https://api.mpas.top/v1"
$env:MEOAVE_LLM_MODEL = "gemini-3-flash"
& $go -C server run ./cmd/meoave-ai-test
Remove-Item Env:MEOAVE_LLM_API_KEY,Env:MEOAVE_LLM_BASE_URL,Env:MEOAVE_LLM_MODEL
```

测试报告写入本地忽略目录：`test-results/go-ai-automation-20261114/summary.json`。
