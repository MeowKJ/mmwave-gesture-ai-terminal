# Meoave 设置保存适配层主机测试

本目录独立构建真实 `../../main/meoave_app.c` 和
`../../main/meoave_model.c`，两者都是独立编译单元，没有复制、替换生产保存、
验证或调度逻辑，也没有通过 TU include 访问/重置生产静态变量。

只替换 ESP 错误/日志/时钟、FreeRTOS 单槽覆盖队列/临界区、NVS、UI、
LVGL 锁、亮度和 USB 写出依赖。`meoave_ui.h`、`bsp_usb.h`、
`fls2_display_diag.h` 使用生产头文件；空 `lvgl.h` 仅满足 UI 公共头的包含依赖。
USB 输出只写内存，不调用串口、板子或驱动。所有源码及生成物均在本目录内；
不要求 ESP-IDF，不修改实施计划或任何生产文件，不执行提交。

## 运行

在 PowerShell 中执行，构建及测试均使用指定 MinGW 工具目录：

```powershell
$env:PATH = 'C:\Apps\Env\mingw64\bin;' + $env:PATH
$adapterTests = 'C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter'
& C:\Apps\Env\mingw64\bin\cmake.exe -S $adapterTests -B "$adapterTests\build" -G Ninja -DCMAKE_C_COMPILER=C:/Apps/Env/mingw64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/Apps/Env/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
& C:\Apps\Env\mingw64\bin\cmake.exe --build "$adapterTests\build" --verbose
& C:\Apps\Env\mingw64\bin\ctest.exe --test-dir "$adapterTests\build" --output-on-failure --output-log "$adapterTests\build\ctest_full.log" -j 4
```

测试使用 C11，全部四个编译单元均启用 `-Wall -Wextra -Werror`。
自定义 `CHECK` 在定义 `NDEBUG` 后仍有效。每个 CTest 用例启动独立进程，
因此适配层静态状态由 C 运行时初始化；不能把多个用例放在同一进程重复执行。
失败检查立即返回非零，未知用例也返回非零。

可单独重现一个用例：

```powershell
& "$adapterTests\build\meoave_app_adapter_test.exe" notice_retry_success
& "$adapterTests\build\meoave_app_adapter_test.exe" read_open_error_feedback
```

完整测试当前为 **50/50 通过**，CTest 返回零。原先两个失败回归继续作为正常测试运行，
不再需要排除；修复前 35/37 的历史结果保留在后文。只检查读取错误及迁移可使用：

```powershell
& C:\Apps\Env\mingw64\bin\ctest.exe --test-dir "$adapterTests\build" --output-on-failure -R 'app_adapter\.(read_|missing_).*'
```

## 覆盖

| 场景 | 用例 |
| --- | --- |
| 初始化前 service 不做工作；空设置；旧配置缺失 app_list 为 GRID；缺失命名空间 | before_ready、defaults、legacy_missing_app_list、missing_namespace |
| NVS 参数越界回默认及有效上下界；读取失败仍继续其他键；非零布尔值归一化 | validation_low/high/min/max、read_errors、boolean_normalization |
| 单槽队列最后值；回调不做闪存或亮度 IO；600000 微秒边界；新值重新计时；到期时新值优先 | debounce_last_value、update_at_deadline |
| NVS 初始化 no-free-pages/new-version/一般错误均不擦除，并反馈失败，设置仍能调整亮度 | init_no_free_pages、init_new_version、init_generic_error |
| RW 打开失败；四个 set 分别失败并短路；commit 失败；句柄关闭；无定时自发重试；下次修改恢复保存 | open_failure、set_brightness/bpm/reduced/app_list_failure、commit_failure |
| 保存成功或失败后 GUI 锁连败三次，只重试通知；open/set/commit/close 不重复 | notice_retry_success/failure |
| 在 set、commit（成功/失败）或获取 GUI 锁期间注入新修改，旧反馈不覆盖新修改；等待旧通知时的新值取消旧通知 | update_during_set、update_during_commit_success/failure、update_during_lock、update_while_notice_waits |
| 状态请求 GUI 锁失败两次后重试；请求合并；初始化前请求保留；通知和状态锁都失败后各自恢复；输出期间的新请求保留 | status_retry、status_before_ready、status_during_notice_retry、status_reentrant |
| 亮度依赖失败记录日志，NVS 保存仍完成 | brightness_failure |
| 三个独立进程：真实保存 → 读取 TYPE_MISMATCH 的启动保持只读 → 正常启动加载原值并恢复保存 | roundtrip_write、roundtrip_read_failure、roundtrip_load |
| NVS 只读打开或 brightness 读取 IO 错误应立即反馈失败；本次启动禁止任何写入，原配置保留 | read_open_error_feedback、read_value_error_feedback（已通过，保留原测试名） |
| 四个键分别注入 ESP_FAIL 或 ESP_ERR_NVS_TYPE_MISMATCH，保留其余可读键；清除故障后两次用户修改仍不得保存，也不能通知成功；GUI 锁失败后只重试失败通知 | read_{brightness,guide_bpm,reduced,app_list}_{io_error,type_mismatch}（8 个） |
| 四个键分别缺失，仅该键采用默认值，600ms 后允许迁移保存全部四键并通知成功 | missing_{brightness,guide_bpm,reduced,app_list}_migration（4 个） |

除用例断言外，桩还检查：队列长度为 1、消息尺寸、接收零等待、临界区平衡、
GUI 锁请求为 5ms、UI 创建/通知/读模型持有 GUI 锁、亮度/闪存写入/USB 输出不持有
GUI 锁或临界区、NVS 命名空间/键名/句柄/模式、成功写入键顺序和值、短路调用轨迹。
任何 `nvs_flash_erase`、`nvs_flash_erase_partition`、`nvs_erase_all` 或
`nvs_erase_key` 调用立即导致用例失败。

## 重启及 fake 边界

`roundtrip_write` 从空 fake NVS 启动，经 UI 回调传入设置 43/11/true/true，
由生产适配层在 600ms 到期时执行真实保存调用。只有 fake `nvs_commit` 成功后，
才写 `build/roundtrip_nvs.bin`；文件只含四个键的存在标志和值，无队列、时钟、
模型、回调或适配层静态状态。writer 先清除上次 fixture 文件，防止残留数据误通过。

CTest 的 fixture 保证三个进程依次运行：writer → read_failure → reader，
上游失败则依赖它的后续进程不运行。中间进程从同一文件加载数据，令 guide_bpm 读取
返回 TYPE_MISMATCH，检查其他键保留、bpm 回退为 6、初始化及后续修改都报告失败；
即使故障注入清除，也不允许本次启动写入。结束时重新读取共享文件，检查仍为
43/11/true/true。

reader 是新的 OS 进程，读取 fake 持久映像，再执行生产
`meoave_app_create()` → `load_settings()`，并检查原四键加载、真实模型初始化
及首次 service 的亮度。随后经 UI 回调修改为 72/7/false/false，在新的 600ms
到期时保存成功，再读文件确认四个值，从而验证正常启动后恢复持久化。
三个进程只共享 fake NVS 文件，绝不共享或重置生产静态变量。
`RESOURCE_LOCK` 避免同次并行 CTest 争用映像。
不要让多个 CTest 实例同时使用同一个构建目录。

逐键故障由 `read_error_codes[KEY_COUNT]` 指定，原 `read_error_mask` 继续提供
多键 ESP_FAIL。每键有独立调用计数，以检查早期读失败后仍读取所有其他键。
错误分支还故意向输出参数写入 55/8/1/1：数值字段均处于有效范围，布尔值非零，
各自均异于该键应采用的默认值。这样可以发现调用方误用错误输出的情况，避免被
后续参数验证掩盖；这项注入不声称真实 NVS 必然在错误时改写输出。

读失败生命周期测试检查全部四键的值和存在标志在每个阶段保持不变，
且所有通知均为失败。它们保留从初始化到两次修改的完整调用计数，确认没有
RW open/set/commit/erase，也没有同次启动的重新初始化或读取尝试。

fake NVS 只模拟确定的 API 返回和成功 commit 后的持久数据。其 staged 写入在关闭时
丢弃不意味着真实 NVS 的失败路径具有事务回滚或掉电原子性；失败链用例只断言调用、
短路、反馈和下次修改可恢复，不对真实闪存部分写入做保证。
UI、RTOS 及锁不运行真实线程；注入点只用于确定性执行相关交错，不能替代并发压力、
真实 LVGL 视觉/触摸、ESP-IDF ABI 或实机重启/掉电测试。

## 实际结果、历史 bug 及修复复审

以下运行均为 2026-11-07，Windows，`C:\Apps\Env\mingw64\bin\gcc.exe` 16.2.0、
CMake 4.4.4、Ninja、Debug 构建，严格编译无警告。

### 历史：修复前 35/37

初轮完整 CTest：**37 个，35 通过，2 失败**。当时两个跨进程恢复用例均通过。
当时失败回归如下，保留此记录说明测试发现的问题：

- `app_adapter.read_open_error_feedback`：已有亮度 37、bpm 9、reduced/list 为 true，
  仅使 NVS_READONLY `nvs_open` 返回 `ESP_FAIL`。初始化回退到默认值，首次 service
  后 600ms 将默认配置写回同一命名空间，通知 `success=true`。
- `app_adapter.read_value_error_feedback`：仅使 brightness 的 `nvs_get_u8` 返回
  `ESP_FAIL`。该键回退为 80，600ms 后覆盖原 37，其他键正常加载，仍通知成功。

两个探针实际均输出：

```text
read error probe: persisted brightness=80 (original=37), notices=1, last_success=1, erase_calls=0
```

修复前根因在生产 `meoave_app.c` 的 `load_settings()`：只有 `nvs_flash_init` 结果更新
`s_storage_ok`，只读打开错误被忽略，四个读取结果也全部丢弃。初始化还会把加载结果
放进保存队列。因此临时 IO 错误与确实不存在的旧配置键没有区别，会静默覆盖原设置，
保存成功反馈还掩盖了加载失败。这里是一个数据丢失/错误反馈 bug 的两条重现路径，
并非自动擦除问题。

### 父 agent 修复及本轮复审

父 agent 随后修复生产 `meoave_app.c`，并报告原有 **37/37 通过**。
本轮只复审生产源码，所有编辑仍限于本测试目录。复审确认：

- 新增 `load_u8` 使用局部变量接收结果，仅 ESP_OK 时更新目标值，错误输出不会污染设置。
- 读取或打开返回 ESP_ERR_NVS_NOT_FOUND 时采用默认值，仍允许旧配置迁移；其他错误
  将 `s_storage_ok` 置 false，初始化立即通知失败。
- 某键读取失败不终止其他键读取，因此仍可保留其余有效配置。
- `save_settings` 在 `s_storage_ok == false` 时提前返回失败，不打开 RW 句柄。
  用户仍能调整本次生效的设置和亮度，但该启动整个生命周期禁止持久化。
- 同次启动不会因随后一次用户修改或依赖恢复而重新启用写入；新的正常进程初始化
  才会重新读取存储并恢复持久化。

本轮新增逐键 IO/类型错误 8 个、逐键缺失迁移 4 个、跨进程读取故障 1 个，
并加强原有多键故障和两个历史回归的完整生命周期检查；正常恢复进程也增加了
再次保存及文件校验。**未发现新的实质 bug**；本轮没有修改生产文件。

### 本轮实际全量结果：50/50

完成上述测试编辑后，使用指定 MinGW 工具链重新配置、编译并执行：

```text
ctest --test-dir firmware/p4_gesture/tests/app_adapter/build --output-on-failure --output-log C:/Users/ijink/Desktop/Projects/meoave/firmware/p4_gesture/tests/app_adapter/build/ctest_full.log -j 4
100% tests passed out of 50
```

CTest 退出码 **0**，**50 通过、0 失败**，三个持久化进程均通过。
全部生产及测试编译单元保持 `-Wall -Wextra -Werror`，没有使用 `WILL_FAIL`、
禁用、排除测试或吞掉失败退出码。此结果是本轮实际运行，35/37 仅为修复前历史。
构建及日志独占使用本目录的 build，执行期间父 agent 按约定未构建该目录。

可复查产物（均在允许目录内）：

- `C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\build\meoave_app_adapter_test.exe`
- `C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\build\Testing\Temporary\LastTest.log`
- `C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\build\ctest_full.log`
- `C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\build\compile_commands.json`
- `C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\build\roundtrip_nvs.bin`

## 新增文件绝对路径

本轮修改其中 6 个文件：CMakeLists.txt、README.md、test_app_adapter.c、
fake_dependencies.c、fake_dependencies.h、stubs/esp_err.h。其余初轮新增文件保持原样。

```text
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\CMakeLists.txt
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\README.md
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\test_app_adapter.c
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\fake_dependencies.c
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\fake_dependencies.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\esp_err.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\esp_log.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\esp_timer.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\esp_lvgl_port.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\lvgl.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\nvs.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\nvs_flash.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\freertos\FreeRTOS.h
C:\Users\ijink\Desktop\Projects\meoave\firmware\p4_gesture\tests\app_adapter\stubs\freertos\queue.h
```
