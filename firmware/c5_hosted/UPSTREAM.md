# 上游来源与改动清单

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/espressif/esp-hosted-mcu |
| 版本 tag | `v2.12.13` |
| 获取日期 | 2026-11-08 |
| 选取理由 | 与 P4 侧 `components/esp_hosted` host **2.12.13** 主次版本完全一致，避免 RPC 协议不匹配 |
| 拷入范围 | 上游 `slave/` → 本目录；上游 `common/` → `./common/` |
| 未拷入 | `host/`、`examples/`、`tools/`、`docs/`（host 侧已有独立的 2.12.13 组件） |

## 重放清单（升级上游时照此改）

1. `main/CMakeLists.txt`
   - `set(common_dir "../../common")` → `"../common"`
   - `COMPONENT_SRCS` 增加 `"c5_io2_guard.c"`
2. `CMakeLists.txt`
   - `set(COMPONENTS ...)` 增加 `esp_driver_gpio`
3. `main/Kconfig.projbuild`
   - 文件末尾追加 `menu "meoave board (V1.0)"`（`C5_IO2_GUARD_ENABLE`、`C5_IO2_GUARD_ASSERT_SDIO`）
4. `main/esp_hosted_coprocessor.c`
   - 增加 `#include "c5_io2_guard.h"`
   - `app_main()` 首行 `ESP_ERROR_CHECK(c5_io2_guard_apply());`
   - `esp_hosted_coprocessor_init();` 之后再调一次
5. `sdkconfig.defaults.esp32c5`
   - 末尾追加 `meoave controller board V1.0` 段（SDIO 传输、板级预设、守卫开关、无 PSRAM、无 self-reset 脚）
6. `main/c5_io2_guard.c` / `main/c5_io2_guard.h`
   - 本项目自有文件，直接保留
7. `README.md`
   - 上游同名文件已改名为 `UPSTREAM_README.md`

## 上游默认值核对结果（v2.12.13）

- 从机源码中 **不存在任何 `GPIO_NUM_2` 引用**（`main/`、`common/` 全查），所以出厂逻辑本来就不会占用 C5 GPIO2；
  守卫的作用是"显式钉死"，防止后续改配置或升级时被意外占用。
- `CONFIG_ESP_SDIO_GPIO_RESET` 默认 `-1`，即从机不复位自己；本板由 P4 驱动 C5 CHIP_PU（GPIO54），符合。
- C5 的 SDIO 从机引脚被 Kconfig `range` 锁死为 CMD=10 / CLK=9 / D0=8 / D1=7 / D2=14 / D3=13，
  与模组接线图、P4 侧 `board_pins.h` 三方一致，**无需也不能改**。
