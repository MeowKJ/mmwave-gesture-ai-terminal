# Meoave 桌面 HID 命令系统

## 目标

桌面端通过原生 USB 的 HID 命令 collection 控制和验收 P4，不依赖 COM3
或 COM10。CDC 仍保留作下载、日志和救援通道；正常桌面自动化优先走 HID。

产品命令目录仅保留真实控制/状态，不再内置固定对话或固定语音演示。
AI 播放使用 `VOICE OPEN` 后的 META/PCM/END 生产协议；`AI STATUS` 报告
真实播放器状态，`AI CANCEL` 取消正在播放的语音。表情预览是用户设置功能，继续保留。

工具入口：

```text
python tools/hid_command.py --list
python tools/hid_command.py --command "LINK CAPS c1"
python tools/hid_command.py --suite safe --report test-results/hid-safe.json
python tools/hid_command.py --suite full --allow-side-effects \
  --report test-results/hid-full.json
```

安装可选主机依赖：

```text
<项目配置的 Python> -m pip install -r tools/requirements-hid.txt
```

## 协议

设备 HID collection 使用 Report ID `4`，每个报告固定 63 字节（不含 HID
Report ID）。加上 1 字节 Report ID 后正好是 USB HID 端点允许的 64 字节：

| 字节 | 内容 |
|---|---|
| 0 | `C` 命令、`R` 回复、`L` 日志、`E` 事件 |
| 1 | 消息 ID |
| 2 | 分片序号，从 0 开始 |
| 3 | 分片总数 |
| 4..62 | UTF-8 文本载荷，最多 59 字节 |

单条消息最多 512 字节、9 个分片。宿主端兼容 Windows hidapi 返回的 64
字节（含 Report ID）和部分后端返回的 63 字节 payload 两种形式。回复、日志
和事件按 `(kind, msg_id)` 独立重组，日志不会把命令回复的分片打断。

## 套件边界

`safe` 只读，不扫描、不播放、不改设置、不复位。

`full --allow-side-effects` 增加 UI 跳转、Wi-Fi 扫描、短试听和麦克风启停，
用于联调。它仍然不会自动执行以下高风险命令：

- `SYS RESTART`、`DOWNLOAD`
- Wi-Fi 密码写入 `WIFI SET`
- C5 复位、烧录、维护命令
- 会改变系统长期配置或造成真实关机的命令

这些命令可以用 `--command` 手动发送，但不应放进无人值守回归。

## 设备端要求

P4 固件必须包含：

- `MEO_LINK_REPORT_ID == 4`，并且 USB 描述符与链路实现使用同一宏；
- `LINK CAPS` 返回链路版本、报告大小、最大消息和最大分片数；
- HID 命令与 CDC 命令走同一个 `dispatch_line`，包括 `SYS` 系列；
- `LINK PING` 返回 `MEO:PONG`，不能只构造字符串而不发送。

## 验收判定

离线协议测试通过不等于实屏、听感或 Wi-Fi 已验收。硬件验收需要在设备
已经运行新固件、屏幕保持正常的情况下运行 `safe`，然后根据需要运行
`full --allow-side-effects`；报告中的 `PASS` 只表示 HID 命令得到期望回复。
