# 当前电脑作为 Go 服务端 · 2026-11-15

## 部署与实现

- `tools/server.ps1 Start/Status/Test/Stop`：本机编译、后台启动、原生 USB 应用端口识别及单轮验收。
- `server/config.local.json` 保存本机地址、模型、语音和凭据，已被 Git 忽略；模板不含真实 key。
- 服务仅监听 `127.0.0.1:8787`，不修改防火墙、不开放公网、不注册开机启动项。
- 新增生产 `/v1/chat`：真实模型→严格句子／16种表情白名单→Go Edge-TTS→48kHz 单声道 PCM→可选 P4 播放。
- 两句共享 generation，逐句递增 sentence_id；先校验全部句子再开始物理播放。
- 修复 `/v1/play` 在合成后提前结束取消上下文的问题；结束时释放 context。
- `meoave-local-test` 使用真实 HTTP 路径，不拼接假模型回复；保存分阶段结果，失败不重试、不覆盖旧报告。
- 不改 P4/C5 固件、显示、旋转、电源保持、GPIO6/GPIO54、音乐或用户音量。

## 已取得证据

本轮证据保存在 Git 忽略的 `test-results/local-server-20261115/`，
服务运行日志及单轮报告在 `test-results/local-server/`。

| 项目 | 结果／边界 |
| --- | --- |
| Go 离线回归与 `vet` | 通过。新增真实 MP3 解码路径的离线静音帧替身测试、全句表情链检查、设备缺失、取消和失败防假成功检查；不是云端或硬件成功证明。 |
| 本机进程 | Start / Status / Stop 经实际运行验证；回环 HTTP 健康检查正常，原生 USB COM10 可打开。 |
| 真实 HTTP TTS | `tts-http.json` 通过：文本「本机服务已经连接，猫猫在这里。」；TTS 2507ms、首个TTS音频包2244ms、解码28ms、重采样1ms；163584帧，48kHz单声道，PCM SHA一致。`synthesized.wav` 是该真实输出，没有在电脑或设备播放。该耗时不含ASR、LLM、设备传输及实际出声。 |
| 原 Gemini 网关 | 一轮 `/v1/chat` 返回上游403，响应称账号服务已停用；报告 `acceptance-20261115-194630-357.json` 为失败。没有声称整轮通过。 |
| DeepSeek 切换 | 用户随后要求换 DeepSeek。新 key 仅发送到官方 `api.deepseek.com`；一次 `/models` 鉴权返回401，响应为密钥无效。`deepseek-auth.json` 保留状态及脱敏错误。未再用这枚 key 重试模型生成，未向旧网关发送该 key。 |
| P4语音下发基线 | 真实 Edge-TTS 后，设备在META阶段返回 `META_REJECTED`，`baseline-play-error.json` 为失败；此轮没有完成真实设备播放。此前的试听 `ESP_ERR_NO_MEM` 记录与本次失败并存，但没有把关联当作已证明根因。 |
| P4状态 | `diagnostics.json` 显示故障记录valid=0、仍在眼睛页、旋转180°、音量70%、麦克风运行、功放关闭；诊断未复位设备。 |

## 新官方凭据实测（2026-11-15 20:00–20:05）

- 用户截图区分了 Tracking ID 与 API Key；新凭据只写入被 Git 忽略的本机配置，没有写入文档、进程参数或报告，也没有发送到旧网关。
- 停止并重启本轮拥有的 Go 服务后，官方 `/models` 返回 HTTP 200；实际可用模型包含 `deepseek-flash`、`deepseek-v4-pro`。继续使用前者，不盲改模型 ID。
- `acceptance-20261115-200106-055.json`：真实 `/v1/chat`、`play:false` 返回 200；模型耗时 1385ms，两句 TTS 和严格表情链／PCM 校验通过，HTTP 总耗时 5315ms。该耗时不代表实际出声延迟；未测试 ASR。
- 设备下发仍失败：`acceptance-20261115-200130-082.json` 及清理旧会话后的 `acceptance-20261115-200343-168.json` 均在等待 `VOICE_OPEN` 回执时超时，HTTP 502、阶段 `device_playback`。云端成功不覆盖这些硬件失败，也不覆盖早期 `META_REJECTED`。
- 修复了 Go 设备读端的真实问题：服务启动后由单独后台 reader 持续排空 CDC 遥测，播放会话期间只分发 `MEO:VOICE_*` 回执；新增设备单测覆盖空闲遥测、分片/合并回复、超长记录和读错误。修复后 `VOICE_OPEN` 不再超时，但实际 META 仍收到 `META_REJECTED`，因此没有把串口修复误报成播音成功。
- `diagnostics-new-key.json`：同一 boot、fault valid=0、眼睛页、旋转180°、麦克风运行、PA关闭；没有把遥测当作目视或听感验收。
- 暂停本轮 Go 服务释放 COM10，经原生 USB 隔离检查：`SYS INFO serial1` 和 `VOICE OPEN serial2` 均收到准确 CDC 回执；随后通过 HID 取消测试会话并恢复 Go 服务。证据 `cdc-isolation.json`，仅证明短命令路径可用，尚未定位长连接回执超时根因。
- 本轮没有修改／烧录固件、复位设备、改音量或操作 C5 引脚；当前电脑继续运行仅回环监听的 Go 服务。
- 修复后的 Go 全量 `test` 与 `vet` 通过；`play:false` 真实 DeepSeek→TTS 仍通过。使用高于诊断代际的 `generation=2147483001` 重试后仍为 `META_REJECTED`，说明问题不只是旧 generation 污染，仍需 P4 固件侧区分 JSON 解析失败、音频资源申请失败或任务创建失败。

## R2 全链路复测（2026-11-15 20:33–20:41）

- P4 新固件从 ROM `COM9` 单次烧录，4/4 镜像均出现 `Hash of data verified`；未重刷、未使用 UART/COM3。
- 固件 ELF SHA：`54d538a780b357d85db814ca85df714aa64b494a36aaab27765eb8804c73b794`。
- 受控 HP CPU-only 复位后恢复 P4 原生 USB `COM10`；运行身份、复位原因 `3`、电源保持和未按键状态均通过。复位末尾 Windows 对串口配置返回了已知的 `PermissionError(13)`，但应用态验证随后成功，未触发盲目重刷。
- 固件离线回归：279 个 Python 测试 + 7 个 HID 协议测试全部通过；Go `test ./...` 与 `vet ./...` 全部通过。
- 服务端真实测试报告：`test-results/local-server/acceptance-20261115-203946-605.json`，DeepSeek `deepseek-flash` → 两句表情链 → Edge-TTS → PCM 校验通过，HTTP 200，`play:false`。
- 设备真实测试报告：`test-results/local-server/acceptance-20261115-204008-701.json`，HTTP 200；两句均收到完整播放回执，`played_frames == reported_frames == frames`，设备错误码为 0，`COM10` 传输和 P4 播放协议通过。
- 本轮仍未把设备回执当作人耳或实屏验收：`human_hearing_verified=false`、`physical_screen_verified=false`、`asr_tested=false`。麦克风上传/ASR 和真实声学对话仍未完成。

## 下一步

1. 官方鉴权、真实模型/TTS、P4 原生 USB、PCM 下发和设备播放回执已经通过；密钥只保留在被 Git 忽略的本机配置，建议后续轮换聊天中曾暴露的密钥。
2. 仍需独立完成用户在场的人耳听感、实屏表情/嘴型目视验收，以及真实麦克风→ASR→LLM→TTS 对话；自动化报告不能替代这三项。
3. 保留旧失败报告，尤其是 `META_REJECTED` 与 `VOICE_OPEN` 超时证据，作为回归基线，不删除历史诊断。

此服务是电脑上的编排服务，模型和 Edge-TTS 仍依赖外网；没有下载本地模型或改用 Python 服务端。
