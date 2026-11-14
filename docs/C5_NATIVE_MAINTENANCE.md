# 自制板 C5 原生 USB 维护

本入口与开发板 UART 专用维护固件分开；不能把无屏开发板 P4 固件烧到自制板。

## 链路和边界

电脑 → P4 原生 USB CDC（303A:4005）→ 专用维护任务 → ESP-Hosted/SDIO → C5。

- `maintenance` 产品 profile 保留原显示/触摸初始化、GPIO29 保持、USB 安全下载/复位、双核 UI、单次 180° 旋转。
- 直接编译复用 `bringup/main/main_maintenance.c` 的 RPC/OTA 状态机，不另写一套升级算法。
- USB 收包回调只验证/排队，阻塞 SDIO/RPC 在独立任务运行。单条事务最多一个在途，不能在 TinyUSB 回调里等 RPC。
- 每条 `C5M <随机ID> <命令>` 都有匹配的 BEGIN/END 边界；END 只是完整传输标记，不等于命令或升级成功。
- 接收上限 1400 字节，支持 512B 十六进制 OTA 数据；超长输入整行丢弃，绝不把尾部解释为下载/复位命令。
- ATTACH 不复位 C5；CONNECT 才请求一次共享 GPIO54/EN 复位。仅适用于当前音频/雷达未启用的产品 profile。
- GPIO6/PIN 与 NET 命令暂拒绝；产品音频仲裁、Wi-Fi UI/凭据和业务联网另验，不继承开发板验收。
- P4 重启只走现有 `SYS RESTART`，不调用无屏程序的 `P4RESET`。
- C5 尚未安装维护服务时，无法用该服务给自己首次安装；必须保留首次下载/备份路径。

## 命令（仓库根目录）

```powershell
python tools/meoave.py build --profile maintenance

# --build-dir 使用绝对路径，或相对 firmware/p4_gesture 的目录名。
# 只更新 P4，不写 C5。
python firmware/p4_gesture/flash_usb.py --build-dir build_meoave_maintenance --report firmware/p4_gesture/test-results/native_p4_flash.json

# 默认不复位 C5。报告使用新文件名，禁止覆盖旧证据。
python tools/c5_native.py check --port COM10 --report firmware/p4_gesture/test-results/native_c5_check.json

# 仅在明确允许一次 C5 EN 复位、音频/雷达停用时使用。
python tools/c5_native.py check --port COM10 --reset-c5 --report firmware/p4_gesture/test-results/native_c5_connect.json

# 仅适用于已经装好维护基线的 C5；不是首次烧录方法。
python tools/c5_native.py update --port COM10 --report firmware/p4_gesture/test-results/native_c5_update.json
```

构建目录独立为 `build_meoave_maintenance`，机器可在忽略的 `tools/config.toml` 设置 `[build] maintenance_dir`。
不修改原 product/hosted/programmer 缓存；首次需要编译新依赖，后续增量编译。
USB 工具以 VID/PID、完整 P4 ELF、随机事务 ID 和 C5 实时 RPC 为判据，不把端口枚举或缓存 IDLE 当作 C5 正常运行。
