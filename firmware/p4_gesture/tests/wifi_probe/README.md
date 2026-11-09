# Wi-Fi CMD5 生产模块独立安全审查 / 主机 C 测试

2026-11-07。审查结论：**当前 `main/bsp_wifi_probe.c` 未发现实质 P1/P2；
在本文明确的范围内，无部署阻断项。**

本目录直接编译真实生产文件 `../../main/bsp_wifi_probe.c` 为独立 object / translation
unit，并链接 `test_wifi_probe.c`。使用生产 `bsp_wifi_probe.h` 和 `bsp_usb.h`；没有
复制生产函数、include `.c`、导出或重置生产 static。每个场景运行在独立进程，生产
初始状态仅由 C runtime 初始化。场景内的后续请求用于验证真实状态转换与再次探测。

本次仅写入 `tests/wifi_probe/`，没有访问任何 COM 口、刷机、构建固件、提交或修改
其它目录。父 agent 的固件编译/部署不属于本次主机测试证据。

## 实际结果

指定 MinGW GCC 16.2.0、CMake 4.4.4、Ninja 1.13.2；C11，所有测试与生产编译单元
均启用 `-Wall -Wextra -Werror`。

| 构建 | 结果 |
| --- | --- |
| Debug | **90/90 通过，0 失败** |
| Release（`-O3 -DNDEBUG`） | **90/90 通过，0 失败** |

自定义 `CHECK` 不受 `NDEBUG` 影响，失败立即退出非零。未设置 WILL_FAIL、跳过或
排除用例；缺少/未知场景名退出 2。父 agent 也独立复跑了 Debug / Release，各
90/90 通过。最后一次 harness 整理后，本审查再编译并运行两种配置，结果如上。

生产文件审查前后 SHA256 一致：

```text
bsp_wifi_probe.c  7B02B0A00225D9FADC39ADD340510FA8CB870EAC8AED6E57E01BD631AD180002
bsp_wifi_probe.h  25B1B46E8ABB2A81322E47388FA6B0303D88B37EAA17932454591D51A740EA61
```

## 运行

PowerShell，仅使用本目录的主机构建目录：

```powershell
$env:PATH = 'C:\Apps\Env\mingw64\bin;' + $env:PATH
$wifiTests = 'C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\wifi_probe'
$wifiBuild = "$wifiTests\build_host_mingw"
& C:\Apps\Env\mingw64\bin\cmake.exe -S $wifiTests -B $wifiBuild -G Ninja -DCMAKE_C_COMPILER=C:/Apps/Env/mingw64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/Apps/Env/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& C:\Apps\Env\mingw64\bin\cmake.exe --build $wifiBuild --verbose 2>&1 | Tee-Object -FilePath "$wifiBuild\build_compile.log"
if ($LASTEXITCODE -ne 0) { throw 'host build failed' }
& C:\Apps\Env\mingw64\bin\ctest.exe --test-dir $wifiBuild --output-on-failure --output-log "$wifiBuild\ctest_full.log" -j 4
if ($LASTEXITCODE -ne 0) { throw 'host tests failed' }
```

Release 将 `$wifiBuild` 改为 `"$wifiTests\build_host_mingw_release"`，并将配置参数改为
`-DCMAKE_BUILD_TYPE=Release`。请勿让多个 agent 同时写同一个构建目录。

单独复现：

```powershell
& "$wifiTests\build_host_mingw\wifi_probe_test.exe" cmd_timeout
& "$wifiTests\build_host_mingw\wifi_probe_test.exe" create_hidden_nomem
& "$wifiTests\build_host_mingw\wifi_probe_test.exe" cleanup_timeout_remove
& "$wifiTests\build_host_mingw\wifi_probe_test.exe" hook_X
```

## 90 个场景覆盖

| 要求 | 场景 / 实际断言 |
| --- | --- |
| 无启动探测 | 每个进程先调用 service 8 次及 STATUS：无任务、计时器、host、slot、transaction 或 GPIO 操作；`startup` 单独验证 IDLE / NOT_RUN |
| USB 回调只排队 | 每次真实 command 调用前后比较任务、host、slot、transaction、cleanup、GPIO、timer 计数；依赖桩还强制 task creation 只能处于 service、所有硬件调用只能处于 worker；USB 写入不持有 portMUX |
| 一次任务、拒绝重复 | QUEUED 和 RUNNING 中发送第二个 PROBE，回执 accepted=0，原 token 不变；重复 service 不创建额外任务；每个 worker 必须 self-delete |
| worker 调度交错 | `hook_T/C/A/X/R/D/1/2/3` 在 task create、host、slot、transaction、remove、delete、三次 GPIO cleanup 中插入 STATUS、重复 PROBE 和 service，仍只运行原探测；`eager_worker` 模拟 task 在 xTaskCreate 返回前完成 |
| 严格 token / 格式 | 30 个 `input_*` 场景覆盖 NULL、前缀/大小写、缺 token、17/40 字符、额外参数/行、分隔 token、标点、非 ASCII 和控制字符、STATUS 的非法 token；非法输入无回复/无状态改变，随后合法请求仍可执行；1/16 字符、数字和字母数字 token 合法 |
| 单条无数据 CMD5 | 每次 transaction 独立断言数值 opcode=5、arg=0、timeout_ms=250、flags=0x1030；data=NULL、datalen/buflen/blklen=0，无 voltage callback，初始 response/error 为零；精确事件串确保每个 worker 最多一次 transaction |
| 严格引脚 | slot=1、SD_MODE_NORMAL、width=1、D0=14、CLK=18、CMD=19；D1–D7、CD、WP 全部 NC；internal_pullup=1、wp_active_high=0。cleanup 只能按 14/18/19 顺序操作，所有其它引脚（包括 54/6/29、屏/触摸）均不允许 |
| 新 IDF 6.1 API | opaque controller/slot handles，create/add/remove/delete/transaction 的真实接口形状；对照本机 v6.1 public headers 的字段、类型、错误码及 flags，未使用 legacy host/global API |
| create 失败与隐藏资源 | busy / no_mem / invalid_arg / timeout / generic 均 DONE + blocked=1；`create_hidden_nomem/timeout` 显式模拟无返回句柄却残留内部资源。禁止 add_slot、transaction、delete、GPIO cleanup；连续三次后续显式 PROBE 仍拒绝 |
| add_slot 失败 | no_mem / invalid_state / invalid_arg / generic：无 transaction、无 remove_slot，删除自有 controller 后释放三脚；delete / GPIO 二次失败分别锁定 |
| transaction 与 cmd.error | 两条独立错误通道分别覆盖 timeout / CRC / invalid_arg / generic；专门验证 transaction 返回 ESP_OK 但 cmd.error 非零；双错误时保留 transaction 返回错误优先级；timeout=NO_RESPONSE，其他=TRANSPORT_ERROR，不采信伪造 R4 |
| R4 | zero / all ones / 无 functions / 无电压 / ready-only / funcs-only -> INVALID_R4 + INVALID_RESPONSE；ready=0 仍可为合理查询回复，1/7 functions、单电压位也覆盖；只读取 R4 的第一个 response word |
| cleanup 与禁止重试 | remove 失败不继续 delete、不碰 GPIO；delete 失败不碰 GPIO；任一 GPIO reset 失败仍尝试余下两脚并记录错误；三类都 blocked。包含 timeout / cmd.error / INVALID_R4 与 cleanup 组合错误，主错误和 cleanup 分开保留 |
| 资源释放与可重试 | 所有无 blocked 的非启动场景都会继续发起一次全新显式探测并成功；包括正常、timeout、transport、无效 R4、可安全清理的 add_slot 失败、task 创建失败；完成后反复 service 不自动重试 |
| task / USB / 时间边界 | task 创建失败无硬件调用、无 self-delete，报告 TASK_ERROR / NO_MEM，下一次显式请求可成功；USB write 返回 NOT_FINISHED 不改变探测状态；slow_driver 模拟耗时超过 250ms，仍检查传入 timeout=250 并如实报告 elapsed |

每个 worker 的依赖调用顺序以事件串检查：

```text
T = xTaskCreate    C = create controller    A = add slot    X = CMD5 transaction
R = remove slot   D = delete controller    1/2/3 = reset GPIO 14/18/19
V = vTaskDelete(NULL)
成功：TCAXRD123V；create 失败：TCV；task 创建失败：T。
```

桩逐步记录 controller、slot、隐藏分配的存活状态。未知句柄、重复释放、slot 尚存
时删除 controller、controller/slot 尚存时 reset GPIO、锁内调用依赖都会失败。
portMUX 桩检查唯一锁对象、进入/退出配对、禁止嵌套；所有外部依赖不得在锁内运行。

## 独立源码审查依据

审查使用当前生产文件和本地 SDK：
`D:/Env/.espressif/v6.1/esp-idf/`。关键比对位置：

- `components/esp_driver_sd_intf/include/driver/sd_host.h`、`sd_types.h`：新 handle API。
- `components/esp_driver_sdmmc/include/driver/sd_host_sdmmc.h`：controller / slot 配置。
- `components/sdmmc/include/sd_protocol_types.h`、`sd_protocol_defs.h`：完整 command 字段、R4 flags / macros。
- `components/esp_driver_sdmmc/src/sd_host_sdmmc.c`：add_slot 初始化 400000 Hz，width=1 的 IO 配置只配置 CLK/CMD/D0；D1–D7/CD/WP 的 NC 参数不会被扩展成其它 GPIO。
- `components/esp_driver_sdmmc/src/sd_trans_sdmmc.c`：默认 probing frequency 400kHz 和 1-bit bus，在 transaction 时设置。
- create 中 claim 后 DMA 分配失败的直接 return、controller reset 失败的直接 return，都可能跳过释放且不返回 handle；因此所有 create 错误均 blocked 的策略合理。桩不会虚构一个可安全删除的句柄来掩盖这一点。
- 生产 `fls2_official_board.c` 的 command 入口转交本模块，service 位于 app_main 循环；这一接线只做只读源码核查，没有把整个 USB/UI/app_main 集成编译进本测试。

主机生产 object 的 `nm --undefined-only` 人工核查仅包含 C runtime、允许的
USB write / timer / 新 sd_host API / gpio_reset_pin / task / portMUX 桩。
没有 reset、flash、card_init、legacy host、屏或触摸外部符号。测试不链接这些接口，
新增此类外部依赖通常会因未声明或未定义而编译/链接失败。

生产实现会正确区分 `sd_host_slot_do_transaction()` 返回值与 `cmd.error`。cleanup
失败不覆盖主错误，且不会继续冒险操作下一层资源；资源归属不明时不会尝试 reset
或重新探测。TASK_ERROR 无 host 所有权，允许下一次显式请求符合资源安全边界。

## 真实测试边界

- 测试验证真实生产 C 在受控依赖返回下的行为；并未运行 IDF driver / 硬件。
  400kHz / 1-bit 的默认行为来自上述本地 driver 源码核查，未通过波形或仪器实测。
  这些 stubs 是接口模拟，不能证明 ESP32-P4 ABI、DMA、cache、interrupt 或 GPIO matrix 的实际行为。
- portMUX 是确定性锁模型；注入调用是可复现交错，不能替代 FreeRTOS SMP / 真实线程竞争测试。
  未证明任务在任何调度状态下都能及时获得 CPU，也未做 watchdog 压力测试。
- 250ms 是传给 transaction 的事件 timeout；并非 controller 创建、锁等待、驱动内部
  reset、transaction、cleanup 或整个 worker 的墙钟硬上限。slow_driver 明确模拟超时
  参数不等于总耗时上限。USB/UI 不等待 worker 是源码/调用边界结论，非实机延迟证明。
- 不发送 CMD0、IO reset、card_init、CMD52 写、烧写或 Hosted handshake；只允许显式的
  CMD5(arg=0) 查询。驱动内部对 SDMMC 外设自身的 reset 属于 host API 初始化，不能将
  `reset=0` 解读为“底层从未复位 SDMMC 外设”；本模块不复位芯片、协处理器或整板。
  GPIO cleanup 的 gpio_reset_pin 仅对 14/18/19 恢复引脚默认配置。
- CMD5 查询不构成冷启动协议完整初始化。R4 只检查“非全 1、有 function、有电压位”
  的最低合理性，不能据此认证响应源、协处理器型号、Hosted 固件、协议完整性或 Wi-Fi 可用。
- NO_RESPONSE 只代表这次尝试没有得到可接受回复，不能推导无 Hosted、芯片损坏或未装芯片。
  所有状态回复保持 hosted=NOT_VERIFIED / wifi=UNAVAILABLE。
- USB 桩验证尝试写入的完整记录，不验证真实 TX admission、TinyUSB/FIFO、断连、主机收包
  或部署脚本。本次没有运行父 agent 的 `probe_wifi_sdio.py`，没有任何实机探测证据。
- create 隐藏资源和 cleanup 失败场景故意保留未知/未释放资源并 blocked；测试验证停止后续
  操作，而不宣称在无法安全清理的情况下实现了资源完全释放。worker 自身仍必须 self-delete。

## 改动与证据路径

根目录：`C:/Users/ijink/Desktop/Projects/meoave/firmware/p4_gesture/tests/wifi_probe/`

- `test_wifi_probe.c`：主机 harness、驱动/task/portMUX/USB 桩实现及全部场景。
- `CMakeLists.txt`：生产独立 translation unit、C11 严格编译、90 个独立进程 CTest。
- `stubs/`：11 个最小接口头文件（ESP 错误、GPIO、新 sd_host、protocol、timer、FreeRTOS）。
- `README.md`、`.gitignore`：复现步骤、审查依据、边界、忽略主机构建产物。
- `build_host_mingw/` 与 `build_host_mingw_release/`：各自的 `wifi_probe_test.exe`、
  `compile_commands.json`、`build_compile.log`、`ctest_full.log`、
  `Testing/Temporary/LastTest.log`。生产文件的实际路径和独立编译命令可复查。
