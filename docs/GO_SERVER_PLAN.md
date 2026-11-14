# Meoave Go 唯一服务端目标

更新时间：2026-11-14

## 目标

将当前电脑作为 Meoave 的本机服务端，生产链路只使用 Go：

```text
P4 录音/设备状态
        ↓（未来设备链路）
Go 会话层 → ASR/LLM 适配层 → Go TTS 适配层
                               ↓
                       Edge-TTS（当前默认）
                               ↓
                 MP3 解码 → 单声道 → 48 kHz PCM16
                               ↓
                 句子 JSON + 表情链 → P4
```

本阶段不刷写 P4/C5，也不把 Python 作为运行时依赖。Edge-TTS 通过 Go WebSocket 客户端调用；Python 基准脚本只作为历史对照，不是服务端入口。

## 当前实现

- `server/cmd/meoave-server`：单一 Go 进程，默认监听 `127.0.0.1:8787`。
- `server/internal/tts`：Edge-TTS Go 客户端，默认 `zh-CN-XiaoxiaoNeural`。
- `server/internal/audio`：Go MP3 解码、立体声转单声道、线性重采样至 48 kHz、S16LE 编码。
- `server/internal/protocol`：严格限制句子、generation、sentence_id、16 种表情和 cue 时间线。
- `server/internal/session`：新 generation 取消旧任务，避免旧音频/旧表情复活。
- `server/internal/llm`：OpenAI-compatible Gemini 客户端；密钥只从环境变量读取。
- `server/internal/api`：`/healthz`、`/v1/synthesize`、`/v1/cancel`。
- `server/cmd/meoave-ai-test`：一次性自动检查 Gemini JSON 表情链，再用 Go Edge-TTS 和 PCM 时轴验证。

## API

```http
POST /v1/synthesize
Content-Type: application/json
```

```json
{
  "generation": 1,
  "sentence_id": 1,
  "text": "你好，我在这里。",
  "expressions": [
    {"at_ms": 0, "expression": "CURIOUS", "intensity": 0.5, "transition_ms": 160},
    {"at_ms": 700, "expression": "RELIEVED", "intensity": 0.65, "transition_ms": 200}
  ]
}
```

返回的是设备可消费的完整句子：48 kHz、单声道、`pcm_s16le`、PCM Base64、帧数、SHA-256 以及 TTS/解码/重采样耗时。第一版选择完整 JSON 是为了先完成可审计的本机闭环；接入设备后再增加二进制流/WebSocket，不改变句子协议和 generation 语义。

## 运行与验证

```powershell
$go = "C:\Users\ijink\Desktop\Projects\meoave\tools\.venv\go-runtime\go\bin\go.exe"
& $go -C server test ./...
& $go -C server run ./cmd/meoave-server
Invoke-RestMethod http://127.0.0.1:8787/healthz
```

Gemini 自动化检查：

```powershell
$env:MEOAVE_LLM_API_KEY = "<本机环境变量，不写入仓库>"
$env:MEOAVE_LLM_BASE_URL = "https://api.mpas.top/v1"
$env:MEOAVE_LLM_MODEL = "gemini-3-flash"
& $go -C server run ./cmd/meoave-ai-test
Remove-Item Env:MEOAVE_LLM_API_KEY,Env:MEOAVE_LLM_BASE_URL,Env:MEOAVE_LLM_MODEL
```

该命令依次检查：Gemini 请求成功、严格 JSON、每句非空表情链、16 种表情白名单、首 cue 为 0、Go Edge-TTS 成功、48 kHz PCM 帧数与 cue 时间线一致。密钥不会写入日志、报告或 Git。

一次真实 Edge-TTS 测试需要网络，运行时不需要 Python、FFmpeg、系统 Go 或设备固件。未提供 API key；Edge-TTS 是当前候选云端 TTS，不承诺长期服务 SLA。IndexTTS2/CosyVoice 留作后续 Go 服务通过 HTTP/gRPC 调用的可替换上游，不在本阶段强行放入设备端。

## 验收边界

已完成的本机门槛：

1. 空文本、无表情链、越界强度、cue 乱序和过渡重叠被拒绝。
2. 24 kHz MP3 被转成 48 kHz 单声道 PCM16，帧数和哈希可复现。
3. generation/sentence_id 原样保留，新 generation 可取消旧合成。
4. Go 单元测试通过后，才允许做一次真实 Edge-TTS 请求。

已完成一次 Gemini → 表情链 → Go Edge-TTS → PCM 的本机自动冒烟；证据在 `test-results/go-ai-automation-20261114/summary.json`。尚未宣称完成：P4 上传、麦克风 ASR、P4 音频下发、实机声画同步、真实 IndexTTS2 GPU 部署。
