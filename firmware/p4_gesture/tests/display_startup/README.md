# Display startup 主机 mock 单测

直接将 `../../main/fls2_display_startup.c` 编译为独立翻译单元，使用真实的
`fls2_display_startup.h`。每个 CTest 场景独立启动进程，由 C 运行时初始化
生产代码的静态状态；测试不包含生产 `.c`、不暴露或重置 `s_state`。

## 构建与运行

PowerShell，在仓库根目录运行：

```powershell
$suite = 'C:/Users/ijink/Desktop/Projects/meoave/firmware/p4_gesture/tests/display_startup'
& 'C:/Apps/Env/cmake/bin/cmake.exe' -S $suite -B "$suite/build" -G Ninja '-DCMAKE_C_COMPILER=C:/Apps/Env/mingw64/bin/gcc.exe' '-DCMAKE_MAKE_PROGRAM=C:/Apps/Env/ninja/ninja.exe'
& 'C:/Apps/Env/cmake/bin/cmake.exe' --build "$suite/build" --verbose
& 'C:/Apps/Env/cmake/bin/ctest.exe' --test-dir "$suite/build" --output-on-failure
```

生产代码和测试均以 C11、`-Wall -Wextra -Werror` 编译。也可直接使用 GCC
（先创建上述 `build` 目录），例如运行一个 warm 场景：

```powershell
& 'C:/Apps/Env/mingw64/bin/gcc.exe' -std=c11 -Wall -Wextra -Werror "-I$suite/stubs" "-I$suite/../../main" "$suite/test_display_startup.c" "$suite/../../main/fls2_display_startup.c" -o "$suite/build/display_startup_direct.exe"
& "$suite/build/display_startup_direct.exe" warm
```

## 覆盖范围

- 初始状态、cold 全零寄存器、warm 继承的非零寄存器、保留位非零但关断位为零。
- GPIO 0 与 63 的安全掩码边界；负数、`INT_MIN`、64、`INT_MAX` 和模拟的
  不可输出 GPIO 均返回 `ESP_ERR_INVALID_ARG`，无 GPIO/RCC/寄存器操作。
- `gpio_set_level` 和 `gpio_config` 失败原样返回错误，立即停止，不读写模拟
  寄存器、不延时、不改变启动状态；修正错误后可使用另一个 GPIO 重试。
- 精确调用顺序：GPIO 先写低再配置输出；RCC 范围内先开 DSI bus/APB clock
  再关 DPI clock；获取 host 0；记录 before；关 host；关 PHY；记录 after；
  检查两个关断位；延时；最后设置 `prepared`。
- host、PHY、两者关断位分别保持为 1 的读回失败均返回
  `ESP_ERR_INVALID_STATE`，记录 after、不延时、不设置 `prepared`，之后可重试。
  另测读回失败后又遇到 GPIO 失败时保留之前快照，随后成功刷新快照。
- 成功后再次调用返回 `ESP_ERR_INVALID_STATE`，即使参数无效，也不再做 GPIO
  校验或其他动作；已记录状态和后来改变的模拟寄存器均保持不变。
- 模拟 500 Hz tick：10 ms 应转换为 5 tick；延时回调内检查 after 已记录且
  `prepared` 尚为 false，整个成功调用只能延时一次。

## Mock 边界

仅提供 GPIO、RCC、DSI host/PHY 关断和任务延时所需 API。没有 VCI、板级
电源、DMA 或其他驱动的 stub，新增外部调用会编译/链接失败。GPIO 配置
只允许本次指定引脚的单一掩码。模拟寄存器的其他位及无关内存 canary
必须保持不变；canary 不代表任何真实硬件寄存器。

GPIO 有效性使用特意简化的主机策略：0..63 有效，34 被模拟为不可输出。
这不表示实际芯片允许 GPIO 63 或禁止 GPIO 34。RCC 仅验证调用范围，
不模拟锁或中断。LL stub 只清除模拟的关断位，并可注入“写入未生效”。
直接字段读取不产生事件，通过 LL 写入前和 delay 时的状态断言、返回后的
快照验证读取结果和相应顺序，不能观测全部真实 MMIO 读取。

这些测试只验证主机控制流与内存寄存器模型，不证明物理时钟、复位、电源、
DMA、显示输出或 cold/warm 硬件行为通过。运行不访问串口或设备。

## 文件

- `CMakeLists.txt`：独立 GCC/Ninja 构建、25 个 CTest 场景。
- `test_display_startup.c`：mock 实现、事件日志、故障注入与断言。
- `stubs/esp_err.h`、`stubs/esp_check.h`：错误码与提前返回宏。
- `stubs/driver/gpio.h`：GPIO 参数和有效性校验。
- `stubs/esp_private/periph_ctrl.h`：RCC 范围 mock。
- `stubs/hal/mipi_dsi_ll.h`：最小 host 寄存器模型及 LL 声明。
- `stubs/freertos/FreeRTOS.h`、`stubs/freertos/task.h`：tick 转换与延时声明。
- `.gitignore`：只忽略本目录的 `build*` 构建目录。
- `README.md`：命令、覆盖和模拟边界。
