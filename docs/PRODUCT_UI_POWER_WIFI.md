# 产品固件：电量、长按关机、Wi-Fi

更新时间：2026-11-11。适用范围：`firmware/p4_gesture` 产品 UI（`APP_MEOAVE_UI`），Wi-Fi 需要 `--profile hosted`。

## 电量显示

- `bsp_power_battery_percent()`：`PIN_BAT_AD`(GPIO49) 经 Q4 分压（100K/100K，×2），取 5 次均值，
  再按单体锂电分段曲线换算（4.20V=100% … 3.30V=0%）。`-1` 表示 ADC 不可用，此时首页不画电量。
- `bsp_power_charging()`：等于 `VBUS_ON`(GPIO28) 分压检测，只能说明“插着 USB”，不等于正在充电。
- 首页右上角状态条：Wi-Fi 图标 + 百分比 + 电池条；插着 USB 时数字和电池条转为青色
  （字体里没有闪电符号，用颜色表达，避免引入位图）。
- 采样节流：`meoave_app_service()` 每 2 s 一次，跑在 app 任务里，不在 LVGL 任务里阻塞。

## 长按关机

- 按键：`PWR_DET`(GPIO30)，低有效；`PWR_HOLD`(GPIO29) 高电平维持供电。
- 状态机 `meoave_app_key_tick()`（app 主循环，20 ms 采样）：
  - 短按（< 2 s）：回主菜单 `MEO_HOME`；
  - 持续按住 1.5 s：顶层遮罩弹出「即将关机」+ 圆环 + 5 s 大字倒数；
  - **松手立即取消并恢复界面**（倒数只在按住期间存在，手抖不会误关机）；
  - 按住满 6.5 s（1.5 s + 5 s）后才 `bsp_power_off()`；此时文案变「正在关机」并隐藏「松手即取消」。
- `bsp_power_off()` 先 `gpio_hold_dis(GPIO29)` 再拉低；`power_shutdown_handler()` 用 `s_poweroff`
  阻止 `esp_restart()` 时重新锁存，否则关机会被复位钩子撤销。
- 插着 USB 时 3V3 由 VBUS 维持，关不掉：此时熄灭屏幕、`PWR_HOLD` 保持低，USB 仍可枚举以便恢复。
  真正断电要在拔掉 USB 之后。

## Wi-Fi（ESP-Hosted / C5）

- 分层：P4 `bsp_wifi_hosted.c` → esp_hosted（SDIO 40 MHz 4-bit）→ C5 上的 network_adapter 固件。
- 后台 bring-up，不阻塞显示/USB；开机后用 NVS（namespace `wifi`，键 `ssid`/`pass`）自动重连。
- `WIFI_BAND_MODE_AUTO`：C5 是双频，band 锁死时会扫不到任何 AP。
- 扫描：SCAN_DONE 回调里**只置标志**，取记录放到独立 `wifi_service` 任务（8 KB），
  记录缓冲区是静态数组（16 条）。原因：`esp_event` 任务栈只有 2304 B，
  在里面取 `wifi_ap_record_t[6]` 必然溢出（表现为 `mcause=5`，pc 落在 `esp_event_loop_run`）。
- 扫描不会降级已连接状态；意外掉线会做最多 3 次、间隔 5 s 的重连，成功后计数清零，不做紧凑重试。
- AP 过滤：丢弃空 SSID、含不可打印字符、RSSI ≥ 0 的条目。实测一台环境里 16 条原始记录只有 1~2 条可用，
  其余为空——**未确认是隐藏网络还是 host/slave 记录差异**，所以列表宁少不错。
- 屏幕：首页图标（蓝=已接入、琥珀=连接中、红=失败、灰=未接入）；设置 → 网络页显示状态、SSID、
  「扫描附近网络」按钮和最多 3 条可点列表（点一下用已存密码连接）。
- USB 命令（大小写敏感，token 任意字母数字）：

```
WIFI STATUS <token>               # state/error/ssid/rssi/ip/aps/raw
WIFI SCAN <token>                 # 触发异步扫描
WIFI APS <token>                  # 已缓存的扫描结果
WIFI SET <token> <ssid> <pass>    # 存 NVS 并立即连接
WIFI JOIN <token> <ssid>          # 用已存密码重连
```

`MEO:UI` 行会带 `wifi=OFF|CONNECTING|ONLINE|ERROR`、`battery=<pct>`、`charging=<0|1>`，可直接做断言。

## 已验证（2026-11-11 本板）

- 电池：`battery=93…95 charging=1`，随 USB 插入稳定。
- Wi-Fi：`WIFI SET MEOWPAS ****` → `ONLINE`，`rssi=-58`，`ip=192.168.8.144`；
  `SYS RESTART` 后 boot 变化、自动重连同一 IP；扫描期间状态保持 `ONLINE`；USB 不再掉线。
- 回归：197 项 unittest 通过（`python tools/meoave.py test`）。

## 未验证 / 边界

- 长按关机的**实机动作**（拉低 PWR_HOLD、屏幕熄灭、电池供电下是否真的断电）尚未做实物确认，
  只用代码路径与编译验证过。
- 屏幕右上角状态条的目视确认、触摸点选网络列表的实机点击尚未验收。
- Wi-Fi 只到“连上路由器并拿到 IP”，没有做 DNS/HTTP 连通性、功耗、长时间稳定性验证。
- 当前 Wi-Fi 产品镜像来自 `--profile hosted`；`--profile product` 仍是关闭 Wi-Fi 的默认镜像。
  C5 维护命令（`C5M …`）与 Wi-Fi 互斥，需要 C5 OTA 时请重烧 `--profile maintenance`。

## 视觉与字库约束（2026-11-11 修订）

- 面板 460×460，**所有可见文字必须是 28px 或 44px**（`meo_font_28` / `meo_font_44`）；
  桌面回归 `assert_visible_glyphs()` 会逐字检查，低于 28px 直接断言失败。
- 字库是清单裁剪的子集（`main/fonts/manifest.json`，185 个符号）。**新增文案必须先补清单再生成**，
  否则字库里没有该字，屏上就是空白或占位框——这次「即将关机 / 按电源键取消 / 扫描附近网络」缺 20 个字，
  就是这样暴露的。检查脚本：改完文案后对比 `manifest.symbols` 与 `meoave_ui.c` 里的字符串。
- 首页视觉契约：眼睛区域外必须纯黑，顶部状态带（y ≤ 120、x ≥ 200）是唯一允许出现 UI 墨迹的位置；
  首页保持单色（电量条的 `+` 表示 VBUS 在位，不代表正在充电）。
- 关机遮罩：全屏不透明黑 + 闭合圆环（`lv_arc`，12 o'clock 起）+ 44px 数字，标题在上、提示在下，
  避免半透明遮罩把下层的「网络」「SSID」透出来。
- 渲染审查入口：`tests/desktop/build/meoave_visual_test.exe test-results/ui-desktop`，
  新增快照 `04a-wifi-scan-list` / `04b-shutdown-countdown` / `04c-shutdown-final`。

## 网络页交互与动画（2026-11-12）

- 每次点击都有可见反馈：`扫描中`、`连接中`、`无保存密码`（提示行 28px、琥珀色、呼吸闪烁，20 s 内自动消失）。
  没有已保存密码时不会再“点了没反应”。
- 进入网络页会自动扫一次；扫描期间再点按钮会被队列拒绝，但提示行仍在闪，不会静默。
- 已连接时扫描实测要 **12–14 s**（C5 双频全扫 + SDIO 回传），把 `scan_time.active` 调到 60/150 ms 也没明显改善，
  所以接收端按“长任务”设计：提示常驻 20 s、旧列表继续显示，等新结果覆盖。
- 连接失败（reason≠0）现在也会做 3 次、间隔 5 s 的重试；此前只有“在线被打断”才会重试，开机首次 join 失败会一直卡在 ERROR。
- 关机页动画：整层 150 ms 淡入、外圈光晕 0.9 s 呼吸（infinity playback）、每次数字变化做一次 310→256 的弹跳；
  收紧到 0 时停止光晕并隐藏「松手即取消」。
- 自动化：`tests/desktop` 内置 `04d/04e` 两个快照与断言——进入页面触发一次扫描、按扫描按钮再触发一次、
  点第一行必须用渲染出来的那个 SSID 回调、回调返回 false 时必须显示“无保存密码”。

## 屏上输入密码（2 寸 460×460 的可行设计）

设计约束：只有触摸、无键盘、**可点目标 ≥ 64 px、文字 ≥ 28 px**、单屏 460×460。
全键盘（26 字母 + 数字 + 符号）至少要 10 列，每键不足 40 px，直接违反两条硬规则，所以做成
**功能机式 T9 多击键盘**：

- 标题「密码」+ 右上模式芯片（`abc → ABC → 123 → !@#` 循环点击切换）；
- 目标网络名显示在输入框上方；
- 输入框 352×64，右侧独立的眼睛键切换明文/掩码；掩码只显示尾部 10 个字符（超出前缀 `…`），
  保证刚输入的字符永远可见；
- 12 个键 99×64：`1..9 0 <- OK` 三行四列；字母模式下键面直接显示 `abc/def/…`（大写模式显示 `ABC/DEF`），
  `1`/`0` 在字母模式分别是 `.` 和 `-`（键面与输出一致，没有“死键”）；
- 多击：同一键 700 ms 内连点循环字母，停手即落字；删除键逐字节退格；
- `OK` 提交：空密码提示「无密码」；提交后立刻把明文从 RAM 清零，再交给适配器存 NVS 并连接。

取舍说明：T9 输入 12 位混合密码约需 20–30 秒，适合“手边没有电脑”的场景；日常首选仍是
`WIFI SET <token> <ssid> <pass>`（一次成型、无输入错误）。两条路径写同一个 NVS 条目。

自动化覆盖（`tests/desktop`，快照 `04f/04g`）：行点击无密码→进入密码页；`2` 连点得到 `A`→`B`；
切到 `123` 后 `1`、`2` 补齐；`OK` 必须把 `MEOWPAS` + `AB12` 原样交给适配器。

## AP 列表解码修正（2026-11-12）

`esp_wifi_scan_get_ap_records()` 批量拷贝回来后只有第 1 条正确，其余为空/乱码（同一原因导致早期只显示 1 个网络）。
改为 `esp_wifi_scan_get_ap_record()` **逐条取**后恢复正常：31 个上报中通过校验 6 条，
`MEOWPAS / 401 / CGJD-4015 / TSQ001 / CU_CGJD-4002 / TP-LINK_338B`。
SSID 是任意 UTF-8，而字库是 185 字子集：新增 `text_renderable()` 逐码点查 `lv_font_get_glyph_dsc`，
不可渲染时回退成「网络 n / 已连接网络 / 已选网络」，不再画占位框。

## 字体：从 Medium 换成"寒蝉圆黑"（2026-11-12）

- 现象：面板 460 px ≈ 2 英寸，28 px 汉字只有约 3 mm 高。`资源圆体 Medium` 笔画偏细，
  `连 / 接` 这类横线密集的字在 3 mm 上发虚，容易被当成"缺字/乱码"。**字库其实一直有这些字**
  （`MEO:UI ... cjk=1` 的设备端自检、以及四套字体的放大渲染都证明 连接 完整）。
- 结论：不是缺 glyph，是字重 + 物理尺寸。改用同为大圆角的 **寒蝉圆黑体 ChillRoundGothic Medium**：
  更粗一档、笔画端头更圆，28 px 下密字不再糊，观感也更"可爱"。
- 可选字体（`python tools/build_fonts.py --set <name>`，`--list` 查看）：`chill`（当前默认）、
  `rhrbold`（资源圆体粗体，最清晰）、`yozai`（悠哉手写，最活泼但笔画细）、`wenkai`（文楷，书卷气）。
- 设备端自检：`MEO:UI` 行末尾的 `cjk=1` 表示"连接中"三个字在 28 px 字库里可渲染；
  新增文案后若看到 `cjk=0`，说明该字没进 `manifest.json` 的符号集。

## 麦克风小应用 + 硬件读数异常（2026-11-12）

- 页面：设置 → 麦克风（第四行）。12 条 LED 式电平条（<7 青色 / 7–9 琥珀 / 10–12 红），
  上方显示 `dBFS`，下方显示 `运行 / 待机`；电平按 dB 映射（-60 dB…-6 dB → 0…1），
  上升快、下落慢，10 Hz 从 app 任务喂入，UI 线程只负责画。
- 采集：`bsp_audio_mic_start()` 起一个任务在 I2S RX 上按 10 ms 块算 RMS/峰值，
  `bsp_audio_mic_level()` 提供最新值；开机自动启动。USB 命令：
  `MIC STATUS|START|STOP|MUTE <token> [0|1]`。
- **两块真实问题（不是 UI）**：
  1. 产品镜像原先根本不调用 `bsp_audio_init()`，音频从未初始化（`bsp_audio.c` 也没进产品构建）。
     已补进 CMake 与启动流程。
  2. ES8389 在这块板上应答在 **I2C 0x12**（CE 拉法），而候选列表只有 0x10/0x11/0x20/0x22，
     所以一直判成"没有 codec"。地址是从总线扫描 `acks=0x00040000` 里查出来的，已加入候选首位。
- **仍未解决**：MIC START 后 RMS 恒定在 ~7500（-12 dBFS）、峰值 28k–32k 反复贴满量程，
  且把 ADC 静音（`0x20=0x03`）后电平**毫无变化** → 说明这些样本不是 codec 的 ADC 输出，
  更像是 DIN(GPIO7) 悬空拾取的 I2S 串扰，或 codec 的 ASDOUT 没有真正路由到该脚。
  下一步需要原理图核对 GPIO7 与 ES8389 ASDOUT 的连接，以及 ADC→I2S 的路由寄存器。

## 界面卡顿：实测定位与修复（2026-11-12）

### 先量化，不猜

`MEO:UI` 状态行新增了每帧成本：`frame_ms`（tick 实际间隔）、`busy_us`（tick 逻辑耗时）、
`render_us`（每帧光栅化）、`flush_us`（每帧刷屏）、`refreshes`（窗口内刷新次数）、
`px_s`（每秒失效像素）。窗口为 30 个 tick（约 1 s）。

修复前（设备实测）：

| 场景 | 刷新 | 渲染/帧 | 刷屏/帧 | GUI 占用 |
|---|---|---|---|---|
| 首页（静止） | 31/s | 15.3 ms | 11.1 ms | **≈78%** |
| 设置页（静止） | 31/s | 14.7 ms | 10.5 ms | ≈75% |
| 关机倒计时 | 43–47/s | 25.0 ms | 5.0 ms | **>100%（饱和）** |

关键反常点：每秒失效像素 ≈ 17.6 M（≈ 84 个整屏），但每次刷新只有约 500 px 的“实内容”。
说明不是画得重，而是**同一块被反复判脏**。

### 根因（用 LVGL 失效入口插桩确认）

在 `lv_obj_invalidate_area()` 临时打印类名与区域，桌面构建复现，得到：

```
INV lv_line 5,74 292x312      <- 左眼
INV lv_line 163,74 292x312    <- 右眼
INV lv_obj 0,0 460x460        <- 关机遮罩
```

- 眼睛是 **线宽约 76 px 的圆头折线**，LVGL 会把描边外扩计入失效框（216×236）；
  改线宽时还会把“旧外扩 ∪ 新外扩”一起失效（292×312 = 91 104 px）。
- `update_home()` 每帧对每只眼调用 `set_pos + set_line_width + set_points`，
  即每帧 6 次大区域失效 → 每秒约 550 k px × 30 ≈ 16.5 M px。
- 关机页的光晕用了 `transform_scale`：**缩放的子对象会让父对象（全屏遮罩）整块失效**，
  每帧重画 460×460，25 ms/帧。

### 修复

1. `update_home()` 改为**先算目标值再比对**：位置、线宽、顶点数组、瞳孔都只在真正变化时下发，
   顶点取整到 1 px。静止面部的刷新从 31/s 降到 2–7/s。
2. `mic_tick()` 只在某条电平条的“亮/灭”翻转时改 y/高度/颜色（原来每帧 12 条 × 3 次）。
3. 呼吸页速率文字只在整数位变化时重排（`lv_label_set_text` 每帧重排 30 次是纯浪费）。
4. 关机页光晕：`transform_scale` → **背景透明度**（不再触发父级整屏失效），
   并把呼吸从 LVGL 的 66 fps 动画改为由 33 ms tick 驱动的 **~12 fps** 受控步进
   （圆环是 178 px 的抗锯齿多边形，每帧约 8 ms，不需要 66 fps）；
   圆环同时从 190→178 px、描边 12→11 px。
5. 遮罩可见期间**暂停面部动画**（被不透明遮罩完全挡住，重绘纯属浪费）；关闭时强制一次全量推送。

### 修复后（同一设备、同一口径）

| 场景 | 刷新 | 渲染/帧 | 刷屏/帧 | GUI 占用 |
|---|---|---|---|---|
| 首页（静止） | 7/s | 7.0–8.0 ms | 4.1–4.9 ms | **8.3–8.4%** |
| 设置页（静止） | 1/s | 0.9 ms | 0.3 ms | ≈0.1% |
| 关机倒计时 | 11/s | 7.8 ms | 2.0 ms | **10.7%** |
| 麦克风页（电平跳动） | 14/s | 12.3 ms | 8.3 ms | ≈29% |
| 呼吸页 / 应用页（无动画） | 0/s | — | — | 0% |

### 可复现的调试命令（USB 串口）

- `UI` — 打印含帧成本的状态行；
- `UI GO <eyes|apps|settings|wifi|display|accessibility|mic|password|breathe>` — 切页；
- `UI SHUTDOWN <秒>` / `UI SHUTDOWN -1` — 显示/隐藏关机倒计时遮罩。

注意：`UI GO wifi` 会触发一次真实扫描（12–14 s），测量窗口会包含扫描期间的反射。

## 麦克风无声：原理图 + 实测定位（2026-11-12 04:00）

依据：`~/Downloads/Netlist_Schematic4_2026-11-06.enet`（嘉立创网表）、
`SCH_Schematic4_2026-11-06.pdf`（中控底板 POWER/CORE 页）、
`ESP32-P4C5-Core核心板_使用指南2026.1.6.pdf` 3.2 引脚描述。

### 1. 板上没有麦克风器件（硬件事实）

`ES_MIC1P / ES_MIC1N / ES_MIC2P / ES_MIC2N` 四个网络的全部成员只有：

| 网络 | 连接 |
|---|---|
| ES_MIC1P | H1.27、C31(1uF) → U6 p24 |
| ES_MIC1N | H1.25、C30(1uF) → U6 p23 |
| ES_MIC2P | H1.28、C29(1uF) → U6 p22 |
| ES_MIC2N | H1.26、C28(1uF) → U6 p21 |

即：**只有串在排针与 codec 之间的隔直电容，没有麦克风、没有偏置电阻、没有连接器**。
ES8389 的 24 脚封装里也**没有 MICBIAS 输出**（数据手册 2. PIN OUT：CCLK/CDATA/ADD、MCLK、SCLK、
LRCK、ASDOUT、DSDIN、MIC1P/N、MIC2P/N、LOUTP/N、ROUTP/N、VMID/ADCVREF/DACVREF、电源），
所以驻极体必须外接偏置。

### 2. 固件 bug 一：I2S DIN/DOUT 接反（已修）

核心板引脚表：模块 **21 脚 = GPIO6**、**22 脚 = GPIO7**；网表把
`I2S_ASDOUT`(codec 输出) 接 21 脚、`I2S_DSDIN`(codec 输入) 接 22 脚。
而 `board_pins.h` 写的是 `DOUT=GPIO6 / DIN=GPIO7`，正好反了 → P4 的输入接在 codec 的输入端
（两端都是输入 → 悬空），P4 的输出接在 codec 的输出端（两个输出对打）。
实测 `din=1 / pulldown=0 / pullup=1 → FLOATING`；修正后同一测试变为 **DRIVEN**。

### 3. 固件 bug 二：字节数当样本数（已修）

`i2s_channel_read()` 返回的是**字节数**，电平任务却用它当 `int16_t` 样本个数循环 →
每块越界读 960 个样本（读栈外内存），RMS/峰值全是假的（峰值长期贴 28k–32768）。
修好后真实读数立刻变成 0（数字静音），问题才暴露出来。

### 4. 当前仍无声的直接原因：没有 MCLK

```
MEO:MIC_CLK MCLK=0%  BCLK=50%  LRCK=48%      (连续多次)
MEO:MIC_LINE din=... -> DRIVEN
MEO:MIC_RAW  bytes=64 n=32 0000 0000 ...
```

BCLK/LRCK 由 P4 正常输出，但 **MCLK(GPIO3) 一直是静态低电平**。ES8389 的系统时钟来自
MCLK（`0x02` bit6=1），没有它 ADC 不转换，于是 ASDOUT 虽然在驱动，但送出的全是数字 0。
把 `0x02` 改成 0x00（用 SCLK 当系统时钟）仍然是 0，说明还需要正确的 CLK_MUL/分频组合。

### 5. 已按数据手册修正的寄存器

| 寄存器 | 原值 | 现值 | 原因 |
|---|---|---|---|
| 0x02 MASTER_CLK | 0x00 (系统时钟取 SCLK) | 0x40 (取 MCLK 脚) | 头文件 `ES8389_MCLK_PIN (1<<6)` |
| 0x20 ADC 格式 | 0x03 / 0x00 (24bit) | 0x63 / 0x60 (S16_LE) | `S16_LE = (3<<5)`，与 P4 16bit 对齐 |
| 0x40 DAC 格式 | 同上 | 0x63 / 0x60 | 同上 |

### 6. 驻极体的正确接法（原理图缺的部分）

| 驻极体引脚 | 接到 |
|---|---|
| 源极 / GND | H1.24 (GND) |
| 漏极（信号） | 经 **2.2k–10k 电阻到 H1.23 (CODEC_3V3)**，同时接 H1.27 (ES_MIC1P) |
| 参考端 | H1.25 (ES_MIC1N) 接 GND（板上的 1uF 会把它耦合到 MIC1N） |

有源麦克风模块（VCC/GND/OUT，如 MAX9814/MAX4466）：VCC→H1.23、GND→H1.24、OUT→H1.27，
板上 1uF 已完成隔直，不需要再加偏置。

### 7. 还没解决 / 下一步

- MCLK 为什么不出：需要接 UART0 看 I2S 驱动的日志（`MCLK is pinned to GPIO...` / 时钟不可达的报错），
  或用示波器量 GPIO3。若 SoC 确实无法在该脚输出 MCLK，只能改板或让 codec 用 BCLK 作系统时钟
  （需要 datasheet 的 CLK_MUL 分频表）。
- 诊断命令：`MIC LINE`（线是否被驱动）、`MIC CLK`（MCLK/BCLK/LRCK 占空比）、
  `MIC RAW`（原始采样）、`MIC REGS`（寄存器回读）、`MIC REG <reg> <val>`（单寄存器写）。

### 8. 补充：MIC1N / MIC2P 在排针上对调（2026-11-12 04:40）

实测排针把 **MIC1N 与 MIC2P 互换了位置**，于是 H1 上的实际顺序是：

| H1 脚 | 网表名 | 实际到 codec |
|---|---|---|
| 27 | ES_MIC1P | MIC1P |
| 25 | ES_MIC1N | **MIC2P**（被换到这里） |
| 28 | ES_MIC2P | **MIC1N**（被换到这里） |
| 26 | ES_MIC2N | MIC2N |

结论：**两对差分输入都还完整，只是换了两个脚**：

- 差分对 1 = **H1.27 (MIC1P) + H1.28 (MIC1N)**
- 差分对 2 = **H1.25 (MIC2P) + H1.26 (MIC2N)**

所以不需要改板、也不需要改固件：把参考端从 H1.25 挪到 **H1.28**，就是一对正确的 MIC1 差分输入。
若仍接在 H1.27 + H1.25，等于把 MIC1P 和 MIC2P 两个"正端"接在一起，差分对不成立。

软件侧的兜底（不想动线时）：ES8389 支持单端输入选择
（`InputSelL_SingleMIC1P=5`、`InputSelL_SingleMIC2P=6`、`InputSelR_SingleMIC1N=6`、`InputSelR_SingleMIC2N=5`，
差分 `DiffMIC1P1N=1` / `DiffMIC2N2P=1`），把左声道配成 `SingleMIC1P`、参考端接地即可绕开对调，
必要时还能用 `ADC1_Invert` 做 180° 反相。具体寄存器字段待抓 UART0 日志后确认。

## 双麦克风：两路能量条（2026-11-12 04:50）

采集端按 I2S 立体声交织拆分：**偶数样本 = 左(ADC1)、奇数样本 = 右(ADC2)**，
`bsp_audio_mic_level(int rms[2], int peak[2])` 分别返回；`MIC STATUS` 现在报
`l_rms / r_rms / l_db / r_db / l_peak / r_peak`。

界面（设置 → 麦克风）：两行 13 段能量条，各自带 `L / R` 与 dB 读数，
每段 24×34 px、间距 4 px；<8 青、8–10 琥珀、11–12 红；上升快下落慢（0.55 / 0.12），
只在某段“亮/灭”翻转时才重绘。快照 `04j-mic-level`（L 8 段、R 3 段）。

### 两颗模拟 MEMS 麦的接法（用户提供原理图）

```
U1: VDD←ADC_MICBIAS12  OUT─1uF→ES_MIC1P   GND─1uF→ES_MIC1N
U2: VDD←ADC_MICBIAS12  OUT─1uF→ES_MIC2P   GND─1uF→ES_MIC2N
```

即：**OUT 走 P 端、GND 走 N 端**，codec 以差分方式收单端麦克风。

### 1P / 2N 对调的电学后果

若 `ES_MIC1P` 与 `ES_MIC2N` 在排针/走线上对调，则：

- 通道 1 = `MIC1P`(实为 mic2 的 GND) − `MIC1N`(mic1 的 GND) → **两端都是地，差分恒为 0**，
  这正好解释 `l_rms=0`；
- 通道 2 = `MIC2P`(mic2 OUT) − `MIC2N`(mic1 OUT) → 两颗麦相减，语音会互相抵消。

### 三种处理方式

1. **改线（最简单）**：把对调的两根线换回来，两端差分恢复。
2. **软件单端**：把两个 ADC 通道分别配成 `SingleMIC1P` / `SingleMIC2P`（N 端不参与），
   对调就完全不影响；需要确认输入选择寄存器的字段（HPM 头文件里只有取值 1/5/6，没有寄存器地址）。
3. **凑合用通道 2**：只取 (mic1−mic2)，但对讲时会抵消，不推荐。

### 仍缺的一环：麦克风供电

当前网表（`Netlist_Schematic4_2026-11-06.enet`，108 个器件）里**没有 `ADC_MICBIAS12` 网络**，
`ES_MIC*` 四个网络只连了隔直电容和 H1 排针。也就是说这块板上两颗麦**没有偏置电源**，
即使接线正确也不会有输出。需要在硬件上补：`ADC_MICBIAS12` 由 3V3 经 RC 滤波（或专用 MICBIAS LDO）
供给 U1/U2 的 VDD，并就近加 100nF + 10uF 去耦（截图上已有 C2/C3、C7/C8）。

### 9. 单端输入绕开 1P/2N 对调（已实现，2026-11-12 05:00）

Linux 驱动 `sound/soc/codecs/es8389.c` 的控件定义给出了确切位置：

| 控件 | 寄存器 | 位 | 取值 |
|---|---|---|---|
| PGAL Select（左 ADC） | `0x72` | [6:4] | 1=差分(MIC1P-MIC1N)、5=Line1P、6=Line2P |
| PGAR Select（右 ADC） | `0x73` | [6:4] | 1=差分(MIC2N-MIC2P)、5=Line2N、6=Line1N |
| PGA 增益 | `0x72/0x73` | [3:0] | 0..14，3 dB/步 |
| AMIC/DMIC | `0x6D` bit6 + `0x23`[1:0] | — | 0=AMIC |

按本板 `ES_MIC1P ↔ ES_MIC2N` 对调，初始化表现在写：

- `0x72 = 0x64` → 左 = 单端 **MIC2P**（mic 2），PGA +12 dB
- `0x73 = 0x54` → 右 = 单端 **MIC2N**（mic 1），PGA +12 dB

回读确认：`rx=72 v=64 / rx=73 v=54 / rx=6D v=16(AMIC) / rx=23 v=00`。

运行时可改，无需重烧：`MIC SEL <token> <left> <right> [gain]`（例如 `MIC SEL t 5 6 4`、
差分对照 `MIC SEL t 1 1 4`）。

**结果**：输入选择已正确，但两路仍然 `l_rms=0 r_rms=0`。因此"对调"不是唯一问题——
ADC 本身没有产出（ASDOUT 被驱动但恒为 0）。剩余两个候选原因：

1. **麦克风没有供电**：本板网表无 `ADC_MICBIAS12`，若 U1/U2 的 VDD 悬空，MEMS 麦输出恒为静音；
2. **codec 缺少系统时钟**：实测 MCLK(GPIO3) 占空比 0%，BCLK/LRCK 正常。

用万用表量 U1/U2 第 1 脚（VDD）即可一次区分。

## 扬声器从未被使能（2026-11-12 05:05）

用户追问"有没有给扬声器上电"。电源分两路：

| 功能 | 引脚 | 现状 |
|---|---|---|
| codec 供电 `CODEC_3V3` | **GPIO26 = AUDIO_PWR_EN** → U5(HE9073) CE | `bsp_audio_init()` 一上来就拉高，正常（否则 I2C 不会 ACK） |
| 功放使能 `PA_CTRL` | **GPIO24 = PA_CTRL** → NS4150B CTRL（10k 下拉） | **一直被 `bsp_gpio_out(PIN_PA_CTRL, 0)` 按住，之后没有任何代码打开** |

也就是说喇叭不响的原因不是供电，而是**功放使能从未给过**（当初为了防上电爆音把脚按住，后续没人松开）。

新增：

- `bsp_audio_speaker(bool)`：开/关功放，开时等 25 ms 让功放退出 shutdown；
- `bsp_audio_tone(freq, ms)`：1 kHz 正弦（约 -11 dBFS），先开功放、播完淡出静音再关功放，避免关断爆音；
- USB 命令 `SPK ON | OFF | TONE <ms>`，`SPK STATUS` 回报 `pa_gpio=` 实测电平。

回读验证（PA_CTRL 改成 INPUT_OUTPUT 才能在纯输出模式下回读，否则永远读到 0）：

```
SPK STATUS -> pa_gpio=0
SPK ON     -> err=0 pa_gpio=1      ← 功放已使能
SPK TONE 800 -> queued=1
SPK STATUS -> pa_gpio=0            ← 播完自动释放
```

## 设置页新增"麦克风测试"开关

- `meo_settings_t.mic_test`，NVS 键 `mic_test`，默认关；
- 设置第 4 行改为「麦克风测试」，右侧是自绘开关（72×40 轨道 + 34 圆点），
  开=青色、关=灰色；点开关只切开关（不冒泡到行），点行的其它位置仍进入电平页；
- 开关是**采集的唯一拥有者**：`bsp_audio_mic_start/stop` 由它驱动，开机不再强制启动采集；
- 字库补了 `试`（189 符号），设置页图标数断言 9→8（去掉该行 chevron）。
