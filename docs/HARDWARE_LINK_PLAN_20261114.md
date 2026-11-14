# 硬件联调计划（2026-11-14）

## 当前边界

- 不修改 C5 固件，不操作 C5 下载三脚；本轮只联调 P4 产品固件与当前电脑上的 Go 服务端。
- 原生 USB 仍承担下载、CDC 控制和日志；音频数据使用同一个原生 USB CDC 端口的二进制帧，不依赖 COM3，也不把 PCM 当作文本命令。
- GPIO6 音频、已验证的音频时钟、显示方向、触摸和电源保持不改。

## 数据链路

```text
Gemini/上层句子
       ↓
Go：Edge-TTS → MP3 解码 → 单声道 → 48 kHz PCM16 → SHA-256
       ↓ HTTP /v1/play
Go：COM10 原生 USB CDC，VOICE OPEN + MVO1 帧
       ↓
P4：CRC16/代际/句号校验 → 有界 PCM 队列 → I2S DMA
       ↓                         ↘ 已完成音频帧
 ES8389/扬声器                   表情链、嘴型和眼睛动画
```

## 帧协议

固定 24 字节小端头：`MVO1 / version / kind / flags / generation / sentence_id / seq / payload_len / crc16`。

- `META`：句子 JSON，可拆成多个帧；必须包含完整表情链。
- `PCM`：最多 960 字节的 PCM16 单声道块；P4 在接收期间直接进入有界队列。
- `END`：宣告本句输入结束，P4 等待 DMA 完成后回报 `MEO:VOICE_SENTENCE_DONE`。
- `CANCEL`：取消当前句子、释放音频仲裁和队列。
- 每个 `PCM` 帧被 P4 接受后回发 `MEO:VOICE_PCM_ACCEPTED token=... seq=...`；Go 主机等待对应序号再发送下一帧。不能用 Windows `WriteFile` 返回时间或 `sleep` 推断 USB 字节已经抵达，否则会把 CDC 缓冲一次性冲入设备队列并触发 `PCM_BACKPRESSURE`。

P4 只接受当前会话的 `generation`、`sentence_id` 和连续 `seq`；CRC 或边界错误会关闭会话，不重放半句数据。

## 验收顺序

1. Go 单元测试、`go vet`、服务端构建。
2. P4 增量编译；确认没有破坏原生 USB、触摸、显示、音频自测。
3. 设备端只读 `VOICE OPEN`/坏 CRC/取消测试。
4. 一句短 PCM：收到 `END_ACCEPTED`，完成帧数等于请求帧数，`error=0`，用户能听到声音。
5. 真实 Edge-TTS 一句：表情链、嘴型和音频同时变化。
6. 取消、重复 generation、拔插 USB 及普通程序复位后恢复。

软件日志通过不等于实屏和人耳验收通过；每项分开记录。

## 2026-11-14 联调记录

- P4 产品固件已通过原生 USB 无感下载：应用态 COM10 → 软件 `DOWNLOAD` → ROM COM9 → 三段镜像写入并逐段 Hash 校验 → HP CPU-only reset → 应用态 COM10。
- 本轮固件 ELF SHA-256：`ebfa734d98c32d1a1427fab80de5b13da6e088ddee66ade6ac19c05d1fde9f4`。
- 电源与运行态：`reset_reason=3`、`pad_hold=1`、`power_ready=1`、`key_pressed=0`。
- 设备端 48 kHz / 单声道 / S16LE 2 秒测试音 + `CURIOUS → HAPPY` 表情链已通过：`VOICE_OPEN accepted=1`、PCM 逐帧接受、`played=96000`、`frames=96000`、`error=0`。
- 只读运行态通过：显示 `ready=1`，触摸 ID `0xB7`，`driver_err=0`，PONG 持续递增；未触发黄色花屏或 P4 fault。
- Go 服务端 `go test ./...`、`go vet ./...`、`go build ./...` 均通过。
- 尚需实屏主观确认：扬声器听感、眼睛/嘴巴随播放变化、屏幕持续无花屏。串口通过不替代这三项目视/听感验收。
