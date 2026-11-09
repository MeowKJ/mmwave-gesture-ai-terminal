# USB CDC record admission 生产源码主机测试

本目录直接编译并链接 `../../main/bsp_usb_tx.c`，使用生产 `bsp_usb_tx.h`。
生产源码是独立编译单元；没有复制函数、include `.c`、导出或重置生产静态变量。
每个 CTest 场景启动独立进程；场景内的连续调用用于观察边界修复与锁释放。
只在本目录新增文件和构建产物，不操作串口、板子，不提交。

最少桩覆盖 ESP 错误常量、sdkconfig、ISR、mounted、CDC capacity / queue / flush。
TinyUSB 函数声明对照工作区 managed_components 头文件：queue 返回 `size_t`，
flush 返回 `esp_err_t`、接收 `uint32_t timeout_ticks`。
桩 TX 容量为 512，与创建测试时的固件 sdkconfig 一致；不加载或修改固件构建配置。

## 运行

PowerShell，使用指定工具链和本测试独占的构建目录：

```powershell
$env:PATH = 'C:\Apps\Env\mingw64\bin;' + $env:PATH
$usbTests = 'C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx'
$usbBuild = "$usbTests\build_host_mingw"
& C:\Apps\Env\mingw64\bin\cmake.exe -S $usbTests -B $usbBuild -G Ninja -DCMAKE_C_COMPILER=C:/Apps/Env/mingw64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/Apps/Env/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& C:\Apps\Env\mingw64\bin\cmake.exe --build $usbBuild --verbose
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
& C:\Apps\Env\mingw64\bin\ctest.exe --test-dir $usbBuild --output-on-failure --output-log "$usbBuild\ctest_full.log" -j 4
if ($LASTEXITCODE -ne 0) { throw 'tests failed' }
```

Release 使用同样命令，将 `$usbBuild` 改为
`"$usbTests\build_host_mingw_release"`，配置参数改为 `-DCMAKE_BUILD_TYPE=Release`。
不要让其他 agent 或另一个 CTest 实例同时写入这两个目录。
本测试不会使用父 agent 的固件构建目录。

单独复现一个场景：

```powershell
& "$usbTests\build_host_mingw\usb_tx_test.exe" boundary_write_failure_retry
& "$usbTests\build_host_mingw\usb_tx_test.exe" reentrant_keeps_owner_lock
& "$usbTests\build_host_mingw\usb_tx_disabled_test.exe" disabled
```

生产源码与测试编译单元全部启用 C11、`-Wall -Wextra -Werror`。
自定义 `CHECK` 在 Release 的 `-DNDEBUG` 下仍执行；断言失败立即退出非零。
缺失/未知场景名也退出非零。没有 WILL_FAIL、跳过或排除用例。

## 覆盖与观察方法

| 要求 | 场景 / 检查 |
| --- | --- |
| queue 返回字节数，不是 ESP 错误码 | success_byte_count；byte_count_equals_error 成功入队 257 字节（数值等于 ESP_ERR_NO_MEM）；queue_zero 将返回 0 判为 ESP_FAIL |
| 最小/精确/最大容量及显式长度 | newline_only；exact_capacity；maximum_record（512 字节）；explicit_length 使用无 NUL 终止的数组 |
| 无容量或少一个字节整条拒绝 | capacity_zero、capacity_one_short：无 queue 调用，仍 flush；capacity_consumed：之前入队消耗容量，释放容量后恢复 |
| 非法参数 | invalid_null、invalid_zero、invalid_oversize、invalid_size_max（SIZE_MAX）；invalid_missing_newline、invalid_newline_outside_length、invalid_trailing_space；参数拒绝不访问 ISR/mounted/CDC |
| 会话/挂载/上下文拒绝 | unmounted、closed、isr：不访问 CDC、不 flush；pending_boundary_preflight：待修复状态下这些拒绝和非法参数不丢失边界状态 |
| unexpected short-write | short_write_repair、short_write_one、short_write_last_byte：分别只接收 3/1/6 字节，然后先补一个 newline 再入队下一条；容量恰好等于 repair + record |
| 边界修复无容量及失败均可重试 | boundary_no_capacity_retry、boundary_write_failure_retry：连续两次失败，下一次修复成功；失败时新记录完全不进入 queue |
| 修复后剩余容量不足 | boundary_only_capacity、boundary_record_one_short：只写 newline、不写新记录；下一次成功且不重复补 newline |
| 修复后新记录再次失败 | boundary_then_zero_write：零写入不要求再次修复；boundary_then_short_write：再次短写要求再次补 newline；检查完整字节流 |
| flush 永远零等待，入队成功语义 | 每次 mock flush 都断言 timeout_ticks == 0；flush_not_finished、flush_failure：返回 NOT_FINISHED 或 ESP_FAIL，完整入队仍 ESP_OK；flush_no_capacity、flush_zero_write：flush 不覆盖 admission 错误 |
| 可重入竞争使用真实 trylock 拒绝 | reentrant_capacity、reentrant_write、reentrant_flush；reentrant_zero_write、reentrant_no_capacity；reentrant_boundary_write、reentrant_boundary_record、reentrant_boundary_failure；竞争调用返回 NOT_FINISHED，且没有额外 capacity/queue/flush 调用 |
| 竞争者不能释放拥有者的锁 | reentrant_keeps_owner_lock：同次外层发送在 write 和 flush 各插入一个竞争者，两次都必须拒绝 |
| 获锁后的所有返回路径释放锁 | 每个启用分支场景末尾再执行一次正常发送，检查单次 queue、单次 flush 和精确字节流；边界失败后的重复尝试还单独检查锁可再次取得。trylock 拒绝路径保持拥有者的锁 |
| 禁用 USB 分支 | 独立 executable 将同一生产文件以 CONFIG_APP_USB_HID_ENABLE=0 编译；正常、NULL、SIZE_MAX 输入均返回 NOT_SUPPORTED，所有依赖调用为零 |

桩记录 capacity / queue / flush 的顺序（A/W/F）、每次 queue 的完整输入和
实际接收长度、接收字节流。capacity 按实际接收字节递减；flush 不擅自释放容量，
测试显式模拟消费者腾出容量。short-write 通过 queue 的返回序列注入。
重入直接调用真实 `bsp_usb_tx_record`，不 mock 原子操作或生产锁。
`reset_mock` / `next_call` 仅重置依赖和观察数据，不触及生产状态。

## 实际结果与边界

2026-11-07，Windows，指定 MinGW GCC 16.2.0、CMake 4.4.4、Ninja：

| 构建 | 编译结果 | CTest 实际结果 |
| --- | --- | --- |
| Debug | 成功，-Wall -Wextra -Werror | **44/44 通过，0 失败，退出码 0** |
| Release（-O3 -DNDEBUG） | 成功，-Wall -Wextra -Werror | **44/44 通过，0 失败，退出码 0** |

每轮包含启用 USB 的 43 个场景及禁用 USB 的 1 个场景。本轮未发现上述
admission 合约的失败；生产文件未改动。测试前后生产文件 SHA256 一致：

```text
bsp_usb_tx.c  745FFEAA6775108F461A66619041AAC4523456B84B8A59968185DAE7B0898A59
bsp_usb_tx.h  FF0FF765AC62E7E99714B471AE93ACF9BA585ABC4ACA0EF63FABF81667E20DCF
```

这些结果验证真实生产 admission 逻辑在受控依赖返回下的行为。重入是确定性
调用交错，不是 ESP 双核/真实线程竞争压力测试；主机原子指令也不能验证 ESP
目标的内存模型或 ISR 实现。未验证真实 TinyUSB FIFO、驱动断连时序或 USB 主机
收包。ESP_OK 仅代表完整入队，不保证主机已收到；flush 失败仍成功是本接口的
既定队列语义。补 newline 恢复后续记录的分隔，之前已发出的短行仍可能不完整。

## 文件与证据绝对路径

本轮新增的 9 个源码/配置/说明文件：

```text
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\CMakeLists.txt
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\test_usb_tx.c
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\README.md
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\esp_err.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\sdkconfig.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\freertos\FreeRTOS.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\freertos\portmacro.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\tinyusb_cdc_acm.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\stubs\tusb.h
```

独占构建目录：

```text
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\build_host_mingw
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\usb_tx\build_host_mingw_release
```

各目录均有 `usb_tx_test.exe`、`usb_tx_disabled_test.exe`、`compile_commands.json`、
`build_compile.log`、`ctest_full.log`、`Testing/Temporary/LastTest.log`。
`compile_commands.json` 可复查两种生产分支的真实源码路径及严格编译参数；
Release 的 `build_compile.log` 包含两个生产编译单元和链接命令。
