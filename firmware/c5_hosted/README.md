# c5_hosted — C5 从机固件（ESP-Hosted slave / network_adapter）

这块板上 P4 要联网，必须由 C5 跑 ESP-Hosted **从机**固件，通过模组内部 SDIO 与 P4 通信。
本目录就是那份从机固件的完整工程，基于乐鑫 `esp-hosted-mcu` **v2.12.13**（与 P4 侧
`components/esp_hosted` host 2.12.13 版本号一致），并按本板硬件做了引脚级改动。

上游原始 README 保留为 `UPSTREAM_README.md`。

## 与上游的差异（只有四处）

| 位置 | 改动 | 原因 |
|---|---|---|
| `main/c5_io2_guard.c/.h` | **新增**：把 C5 `GPIO2` 钉成高阻（输入、无上下拉、无中断、永不写输出锁存） | 模组内部 C5 GPIO2 ↔ P4 GPIO6，而本板 P4 GPIO6 是 ES8389 的 I2S DOUT，两者共用一根铜线 |
| `main/esp_hosted_coprocessor.c` | `app_main()` 第一行调用 `c5_io2_guard_apply()`，ESP-Hosted 起来后再调一次 | 保证在任何初始化之前、以及之后都不会被抢走 |
| `main/CMakeLists.txt` | `common/` 相对路径由 `../../common` 改为 `../common`；加入 `c5_io2_guard.c` | 本工程把 `common/` 放在上一级，不再与 `slave/` 平级 |
| `CMakeLists.txt` | `COMPONENTS` 增加 `esp_driver_gpio` | 守卫模块直接使用 GPIO 驱动 |

除此之外没有任何逻辑改动，方便以后整体升级到新的 esp-hosted-mcu 版本。

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
| `build_c5/c5_hosted_slave_merged.bin` | 单文件镜像，从 **0x0** 一次写入（推荐） |
| `build_c5/network_adapter.bin` | 仅 app，偏移 0x20000 |
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

本板 **没有引出 C5 的 UART0/BOOT/USB**（模组 4/5/6 脚全部悬空），P4 也无法控制 C5 的
strap（26/27/28 只在模组 6/7/8 脚上）。所以只能：

1. 模组 **6/7/8 三脚连片接 GND**（进 Joint Download Boot 1，含 SDIO 下载）；
2. 整板断电再上电；
3. P4 侧用 esp-serial-flasher 的 SDIO 端口烧写，或按 `test-results/c5_programming_path.md` 走飞线 UART；
4. 烧完撤掉接地，**再断一次电**（strap 有锁存），C5 才会从新固件启动。

## 验证烧录结果

P4 侧一条命令：

```bash
python tools/meoave.py wifi --no-reset
```

- `result=SDIO_R4` → C5 已在跑 SDIO 从机（进入 Hosted 握手阶段）；
- 仍是 `NO_RESPONSE` → 固件没跑起来或没进下载模式。

GPIO2 是否真的高阻，用 P4 的引脚诊断读三态：

```
# P4 侧 USB 命令：WIFI PINS <token>
# 期望 io2np/io2pu/io2pd = 0/1/0（高阻悬空）
```

## 版本与来源

见 `UPSTREAM.md`。升级上游时按该文件里的 diff 清单重放三处改动即可（第四处是路径）。
