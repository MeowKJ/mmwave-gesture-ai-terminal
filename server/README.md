# Meoave Go Server

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
