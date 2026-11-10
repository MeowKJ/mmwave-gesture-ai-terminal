# c5_hosted — C5 从机固件（ESP-Hosted slave / network_adapter）

这块板上 P4 要联网，必须由 C5 跑 ESP-Hosted **从机**固件，通过模组内部 SDIO 与 P4 通信。
本目录就是那份从机固件的完整工程，基于乐鑫 `esp-hosted-mcu` **v2.12.13**（与 P4 侧
`components/esp_hosted` host 2.12.13 版本号一致），并按本板硬件做了引脚级改动。

上游原始 README 保留为 `UPSTREAM_README.md`。

## 当前边界（2026-11-11）

- 原自制板和新接入、可切换 P4/C5 串口的开发板是不同硬件，不能混用烧录前提。
- 新开发板已实测C5 v1.2/revision102、4MB，Hosted实时RPC和维护身份通过；P4维护UART为COM4/460800。
- 正常维护固件已安装，10轮SDIO A/B更新、中断、未确认/启动失败回退、SDIO失联95秒后单次EN回退通过。
- 当前正常应用ELF为b97127f15c5c60ab3a3981601b345a427c050eebc7ca09b709d5df0adaaff46f。
- 目标是 Hosted 正常运行时经 SDIO 更新非运行 OTA 槽；这不等于固件失效时也能免引脚进入 ROM。
- 最新链路、证据与限制以`../../docs/C5_MAINTENANCE.md`为准，早期bringup文档只作历史记录。
- 开发板维护P4不含自制板电源保持/原生USB/UI/音频仲裁，不能直接迁回；产品集成仍待验收。

## 与上游的差异

| 位置 | 改动 | 原因 |
|---|---|---|
| `main/c5_io2_guard.c/.h` | **新增**：C5 `GPIO2` 默认高阻（输入、无上下拉、无中断）；仅专用维护租约可暂时切换 | 模组内部 C5 GPIO2 ↔ P4 GPIO6，共享 ES8389 I2S DOUT，不能争用 |
| `main/esp_hosted_coprocessor.c` | `app_main()` 第一行调用 `c5_io2_guard_apply()`，ESP-Hosted 起来后再调一次 | 保证在任何初始化之前、以及之后都不会被抢走 |
| `main/CMakeLists.txt` | `common/` 相对路径由 `../../common` 改为 `../common`；加入 `c5_io2_guard.c` | 本工程把 `common/` 放在上一级，不再与 `slave/` 平级 |
| `CMakeLists.txt` | `COMPONENTS` 增加 `esp_driver_gpio` | 守卫模块直接使用 GPIO 驱动 |
| `main/slave_control.c` | OTA 会话检查、失败释放句柄、边界检查、激活错误码及定时器失败处理 | 重复升级和异常不能被当作成功 |
| `main/esp_hosted_coprocessor.c` | UART 启动身份/阶段日志，检查 Hosted 初始化返回值 | 区分 Flash 启动、局部初始化和真实 SDIO 通信 |
| `main/c5_maintenance.c`、共享协议 | 实时完整ELF/boot nonce/槽身份、显式确认、90秒回退、高阻租约；独立reset/hang故障注入构建 | 免下载三脚维护与可审计失败恢复，不靠假健康确认 |
| `main/slave_gpio_expander.c` | 通用GPIO RPC拒绝GPIO2，必须走租约接口 | 防止旁路夺走音频共享网 |

本地修改需在升级上游时重放；软件故障注入测试位于 P4 工程 `tests/test_c5_ota.py`。

## 引脚（硅片固定，三处证据一致）

C5 的 SDIO 从机引脚由芯片决定，不可配置（IDF `sdio_slave_pins.h`）：

| 信号 | C5 GPIO | 模组脚 | P4 GPIO |
|---|---|---|---|
| CMD | 10 | 内部 | 19 |
| CLK | 9 | 内部 | 18 |
| D0 | 8 | 内部 | 14 |
| D1 | 7 | 内部 | 15 |
| D2 | 14 | 内部 | 16 |
| D3 | 13 | 内部 | 17 |
| IO2 | 2 | 内部 | **6（= I2S DOUT，必须高阻）** |
| CHIP_PU | — | 内部 | 54（与雷达 SOP0 复用） |

## 编译

本项目在 Windows + Git Bash 下**不能用 `idf.py`**（IDF 拒绝 MSYS 环境），统一走仓库命令：

```bash
python tools/meoave.py c5            # 增量编译 + 合成镜像
python tools/meoave.py c5 --clean    # 丢 sdkconfig.c5、cmake --fresh 全量重配
```

该命令等价于用 `tools/config.toml` 里的 cmake/ninja 路径手工执行：

```bash
cmake -S firmware/c5_hosted -B firmware/c5_hosted/build_c5 -G Ninja \
      -DIDF_TARGET=esp32c5 -DSDKCONFIG=.../sdkconfig.c5 \
      "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c5"
ninja -C firmware/c5_hosted/build_c5
```

产物：

| 文件 | 用途 |
|---|---|
| `build_c5/c5_hosted_slave_merged.bin` | 上游兼容合并产物；本项目已安装板禁止直接使用，会覆盖分区/应用等。首次维护安装走备份+有限写入工具，不能用于OTA |
| `build_c5/network_adapter.bin` | 运行时 OTA 仅传这个 app；首次按当前分区表烧录时偏移 0x20000，OTA 时由从机选择非运行槽 |
| `build_c5/bootloader/bootloader.bin` | 偏移 0x2000 |
| `build_c5/partition_table/partition-table.bin` | 偏移 0xc000 |
| `build_c5/ota_data_initial.bin` | 偏移 0x16000 |

## 编译实测（2026-11-08）

- 目标 `esp32c5`，链接通过，`network_adapter.bin` = 0x13ee40（1.31 MB），OTA 槽余 34%；
- 单文件镜像 `c5_hosted_slave_merged.bin` = 1 437 248 字节；
- 守卫已确认链入：`nm` 有 `c5_io2_guard_apply`，反汇编 `app_main` 里有两处 `jal c5_io2_guard_apply`
  （初始化前、Hosted 起来后各一次），镜像里能在字符串表看到 `GPIO%d pinned high-Z`。

配置要点（已写进 `sdkconfig.defaults.esp32c5`，不需要手工改 menuconfig）：

- `CONFIG_ESP_SDIO_HOST_INTERFACE=y` — 走 SDIO，不是 SPI/UART
- `CONFIG_C5_IO2_GUARD_ENABLE=y` — GPIO2 高阻守卫
- `CONFIG_C5_IO2_GUARD_ASSERT_SDIO=y` — 编译期断言：任何传输引脚落到 GPIO2 就直接编译失败
- 分区表 `partitions.esp32c5.csv`：两个 1.875 MB OTA 槽

## 烧录前提（重要）

先确认是哪块板、串口连到哪颗芯片、Flash 容量/修订号及备份要求，再明确告知用户烧 P4 还是 C5。
原自制板未接出 C5 UART，P4 也不能控制 C5 的三根下载 strap；其历史 SDIO ROM 下载操作见
工具文档和测试记录。三根 strap 已由用户拆开，**不要再次把它们长期并联**。
新开发板首次UART安装与P4控制C5 EN已实测；后续应用维护固定UART在P4即可，不要求用户切C5或拉下载脚。
原自制板的GPIO54/雷达SOP0共享及GPIO29电源保持仍需产品固件专门集成，不能套开发板维护镜像。
需要重采样 strap 时必须复位 C5，但不必一概整板断电；何时操作由对应硬件流程决定。

## 验证烧录结果

P4 侧一条命令：

```bash
python tools/meoave.py wifi --no-reset
```

- `result=SDIO_R4` 只表示收到 SDIO 基础响应，不证明 Hosted 应用已运行；
- `NO_RESPONSE` 不能单独定位启动模式、供电或引脚问题；
- 真实验收必须有 C5 启动身份、Hosted INIT/版本 RPC，以及重复升级后新身份/健康检查；
- 原板 `WIFI PINS` 旧命令不适用于 Hosted owner，不能靠并行接管引脚证明高阻。

## 版本与来源

2026-11-10 新开发板UART已确认出厂C5v1.2/4MB正在运行Hosted2.12.9（IDF6.0.2），
不是本目录2.12.13候选，尚未给新C5烧录。本候选修订号/Flash范围匹配，但互通/SDIO OTA待实测；
优先保留出厂启动区，通过P4独立Hosted握手确认后再决定是否更新。详见根目录docs/C5_SDIO_OTA_BRINGUP.md。

见 `UPSTREAM.md`。升级上游时重放其清单并执行回归测试。
