# 自制板 FLS2 官方对照固件

这不是“完全未修改的官方固件”。显示主流程来自下载的 OSPTEK
`esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9`，仅四处替换生成到
`build_fls2_source`：LCD RST 改 GPIO12、触摸 SDA/SCL 改 GPIO8/9、
`app_main` 重命名后由板级入口调用。初始化命令头原样复制，所有 LVGL
配置从官方 **sdkconfig** 复制，RGB888 / 单 lane / 500 Mbps / DPI 16 MHz
及 porch、亮度、缓冲策略不改。

额外差异必须保留在诊断记录中：

- `fls2_official_board.c` 使用自制板电源保持和 USB 自动下载；VCI_EN
  GPIO20 设高，触摸 GPIO10 低 10 ms / 高 120 ms 后再调用官方主流程。
- 非 LVGL 配置沿用自制板，包括 flash、PSRAM、USB、任务配置等；完整差异
  在生成的 `audit.json`，并非所有配置都与官方相同。
- **现有 managed CO5300 驱动仍带“跳过 DCS ID 读取”的本地补丁**。
  之前工作报告读 ID 会卡住，本次检查尚未复现此现象；不能据此断言屏幕
  不回 BTA、链路已通或硬件损坏。锁文件 hash 一致也不能证明源码未改。
- CST820 组件源码已逐文件对比，与官方例程相同。
- 不编译旧显示/触摸 BSP、业务 UI、音频、雷达，不做彩条或 I2C 全地址扫描。
  手动基线不自动执行彩条。用户后来要求固化的自动循环由独立配置
  `CONFIG_APP_FLS2_AUTOTEST` 开启，不修改官方初始化流程或参数。

## 独立构建（PowerShell，本机工具路径）

```powershell
$env:IDF_PATH='D:\Env\.espressif\v6.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
$env:IDF_COMPONENT_MANAGER='1'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'
$env:ESP_ROM_ELF_DIR='C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PYTHONUTF8='1'
$env:PATH='C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;'+$env:PATH
$python="$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
Set-Location 'C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture'
& $python prepare_fls2_official.py
cmake -S . -B build_fls2_official -G Ninja "-DPYTHON=$python" "-DSDKCONFIG=$PWD/build_fls2_source/sdkconfig" -DIDF_TARGET=esp32p4
if ($LASTEXITCODE -ne 0) { throw 'CMake failed' }
& $python prepare_fls2_official.py --verify
if ($LASTEXITCODE -ne 0) { throw 'Audit failed' }
ninja -C build_fls2_official
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

不覆盖业务 `sdkconfig` 或 `build`。烧写须使用
`build_fls2_official/flasher_args.json`，不能误用旧 `build`。
现有 `flash_usb.py` 在“写入已校验成功、仅最后 reset 失败”时可能重复尝试
no-stub；应先读日志，不应把 reset/掉电当成写入失败。

## 验收边界

USB 接收 `PING` / `STATUS`；每 2 秒输出 `FLS2:STATUS`。
`ready=1` 仅表示官方函数返回、创建 UI 的步骤结束，不是屏幕亮起证明。
VSYNC 标记 `NOT_MEASURED`：LVGL 注册回调会覆盖先前 VSYNC 回调，旧版
`vsync=2 fps=0` 不能证明硬件停止刷新。不要覆盖 LVGL 的 flush 完成回调。
启动成功不等于持续触摸正常；最终需目视出图和实际触摸事件验证。
新增触摸诊断使用链接包装器，官方 main 和 CST820 源码仍不变。默认只记录
官方驱动实际读事务；`TOUCH` 输出 ID、读成功/失败次数、原始 15 字节数据、
坐标及数据年龄。`TOUCH ON` 每 100 ms 独立读坐标寄存器，每秒读一次
0xA7 ID；`TOUCH OFF` 停止额外读。读事务使用同一 IO 并用互斥锁串行化，
不扫描、不写寄存器、不额外复位。额外读是显式测试行为，可能消费控制器
数据，独立 probe 成功不等于 LVGL 输入任务正常；两类读计数分开报告。
`invalid` 仅统计点数大于 1 或按下坐标超出 460×460 的帧，不代表 I²C
错误；需额外采样区分多点、边缘坐标、映射或数据异常，不能据此直接判坏。
`DISPLAY` 输出主控绘图 API 成功/失败次数；`DISPLAY BARS` 显式启用
DSI 主控硬件垂直彩条，20 秒后自动恢复；`DISPLAY NORMAL` 提前恢复。
彩条绕过 LVGL 帧缓冲，不改官方时序、速率或 LVGL 回调。API 返回成功仅
说明主控接受配置，不表示屏收到信号；是否显示仍需目视确认。
`DISPLAY HW` 只读主控 GPIO 输出寄存器、DSI PLL/模式/错误状态；GPIO 输出
寄存器为高不代表屏端实际电压正常。`DISPLAY COLORS` 用临时 LVGL 全屏
对象依次显示白/红/绿/蓝，各 3 秒，随后恢复官方 widgets。

`DISPLAY PROBE` 是**额外、实验性**的有界 DCS 读回测试：参考本机 IDF 6.1
`esp_hal_lcd/mipi_dsi_hal.c` 的 MRPS + DCS READ 包序列，针对官方配置的
host 0 / VC 0 / 8-bit 命令，通过目标专用 LL API 实现每条指令 150 ms
超时，避免 SDK 无限等待饿死系统。显式调用时取得有界 LVGL 锁，暂时停
DPI bridge、切命令模式，读取 0x04/0x0A/0x0C/0x0F/0x52 后恢复原 host
模式、BTA/VCID 配置和 bridge 使能。读引擎仍 busy 时跳过剩余命令。
不改库源码、不改速率或官方初始化表、不重新注册 LVGL 回调。
这是区别于官方启动行为的诊断方法；读超时只能说明该方法未收到返回，
**不能单独判定屏损坏**，还可能涉及供电、连线、BTA 配置或测试方法本身。
`REPORT QUIET` 暂停周期状态输出以减少既有 512-byte CDC 队列的截断；
不停止触摸、显示或诊断任务。`REPORT NORMAL` 恢复周期报告。
SW1 仅开机：若烧写后的 watchdog reset 使电源保持释放，需要重新按键上电，
不是让 SW1 进入下载模式。

## 固化的上电循环诊断

在已准备好的独立工程上运行 `prepare_fls2_official.py --enable-loop`，然后
按上面的环境执行 `ninja -C build_fls2_official`、`--verify`，烧写独立工程。
该开关只修改生成配置，保留官方生成源及已有审计历史；业务配置不变。
重新运行不带参数的生成模式会重建手动基线，之后需再次 `--enable-loop`。

上电后无需电脑发指令，官方初始化完成即无限重复约 44 秒的调度周期：

1. widgets 正常界面 8 秒，同时记录 GPIO/DSI 主控状态。
2. 白、红、绿、蓝各 3 秒，自动恢复 widgets；预留 2 秒余量。
3. 主控硬件垂直彩条 20 秒，自动恢复；预留 2 秒余量。
4. 输出 `LOOP_END result=SCHEDULE_DONE`，显示 API 计数，然后下一轮。

循环期间每 200 ms 输出 `TOUCH_LIVE`：点数、坐标、ID、实际触摸驱动读
成功/失败数、按下/边沿/越界统计、数据年龄。不额外扫描、复位或写触摸
寄存器；默认观察官方驱动真实读事务。周期日志可用 `REPORT QUIET` 暂停，
但本机 CDC 短回复滞留现象仍可能发生，建议保留 `REPORT NORMAL`。

- `LOOP`：查询启用状态、轮次、阶段、上电配置和是否需重启。
- `LOOP OFF`：暂停并恢复正常 widgets，安全销毁临时测试屏对象。
- `LOOP ON`：恢复循环；重复 ON 不打断当前阶段。
- 手动 `DISPLAY NORMAL/BARS/COLORS/PROBE` 会先暂停循环，避免争用。
- OFF 仅本次运行有效；重新上电仍按固化配置自动循环。
- 纯色未能在时间余量内结束时停止循环并请求恢复，不带病叠加彩条。
- **DCS 读回不进自动循环**。保留显式 `DISPLAY PROBE`，每次上电最多
  实际尝试一次；读引擎忙会报告需重新上电并禁止恢复自动循环。
- 不自动断电、复位屏幕或改速率/初始化表；USB 自动下载入口始终保留。
- `SCHEDULE_DONE` 只表示调度阶段结束，不是亮屏 PASS 或屏幕损坏鉴定。

纯 C 调度器可在本机用 GCC 单测（不连接硬件、不烧写）：

```powershell
gcc -std=c11 -Wall -Wextra -Werror -Imain main/fls2_test_loop.c tests/test_fls2_test_loop.c -o build_fls2_source/test_fls2_test_loop.exe
./build_fls2_source/test_fls2_test_loop.exe
```

## 2026-11-07 全量诊断实测注意事项

- 本机 `REPORT QUIET` 后曾出现 CDC 短回复滞留，恢复 `REPORT NORMAL`
  才读到此前 PONG。这不是无响应或复位的充分证据。本轮采用 NORMAL
  并定期发 PING；部分高密度日志仍截断，只采信完整字段。
- 纯色四步和硬件彩条均完成、恢复普通模式；USB 与触摸持续响应。
  显示是否可见必须单独记录用户反馈。
- 有界读 0x04 在 150 ms 后于 `rx_empty` 超时，随后读引擎仍 busy，
  跳过其余四条。`00,00,00` 是预先清零的缓冲，不是屏返回的 ID。
- `host_config=RESTORED` 只确认恢复配置寄存器；读后 FIFO 的 busy 位
  仍为 1，**不表示读引擎已恢复空闲**。不要重复 probe，也不要把读后
  状态当成未扰动的官方基线；重新做显示基线前应重启/重新上电。
- 主控 int1 bit7 (`dpi_pld_wr_err`) 在初次串口恢复及 probe 后均曾出现，
  正式 probe 前两次快照为 0。不能把它单独归因屏故障或 probe。
