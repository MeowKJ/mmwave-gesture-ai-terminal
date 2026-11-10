# P4/C5 开发板无屏诊断

这是可手动切换 UART 的鱼鹰光电 P4C5 开发板测试程序，不是自制底板的产品固件。
不初始化屏幕、PSRAM、音频、雷达、电源保持或原生 USB，不配置/驱动 GPIO6。
UART0=TX37/RX38、115200；SDIO=CLK18/CMD19/D0..3=14..17；显式 C5RESET 使用 GPIO54。
固定要求 P4 v3.1–v3.99、16 MB Flash。首次实测 P4=v3.2，CH343=COM4。

## 工具

机器地址仍统一使用根目录 `tools/config.toml`。在仓库根目录运行：

```powershell
python tools/p4_bringup.py --build
python tools/p4_bringup.py --flash --port COM4 --capture 10
python tools/p4_bringup.py --port COM4 --capture 8 --command PROBE
python tools/p4_bringup.py --port COM4 --capture 8 --command C5RESET
```

串口必须人工选到 P4。写入使用显式 `--chip esp32p4 --no-stub`：本板首次普通 RAM stub
启动无响应，ROM 直接读写可用。只写 bootloader/分区表/应用，不整片擦除，不写 C5。
分区表按开发板烧录前只读备份重建，保留 NVS/phy_init/factory/storage 的地址、大小和属性。
原 factory 演示应用会被替换；此前仅保存分区表和应用头，不是完整 P4 固件备份。

`--build` 使用独立增量缓存；配置变化只重建本小工程，不清理产品 UI 的构建目录。
捕获阶段不执行自动 DTR/RTS 复位序列，端口打开前请求两线 false；实际电气效果仍取决于硬件。
在匹配 P4 程序 ELF SHA 之前，工具只发 INFO，不发复位/SDIO 操作命令。
命令只发一次，不靠错误后的盲重刷或反复复位掩盖问题。

## 行为与证据

- 开机只运行一次 1-bit/400 kHz PROBE，不复位 C5。
- PROBE 初始化卡/CCCR，然后 CMD52 读取完整 CIS 和 5 次 12 B 共享寄存器。
  不读 FIFO、不清 IRQ，不修改 Flash。卡初始化并非完全被动观测。
- `C5RESET` 显式请求 GPIO54 拉低20 ms后释放，等5秒后 PROBE；未控制三根 strap。
  程序返回成功不是 C5 EN 引脚电气波形的独立证明。
- 每次操作释放 SDMMC owner，释放失败则封锁后续操作；不和 Hosted/下载 stub 并行运行。
- `0092:1017` 是官方 flasher 的 C5 ROM 身份；`0092:6666` 只记原始设备身份，
  不能因此宣称 Hosted 已运行或证明具体 C5 应用/型号。
- 未收到诊断应用签名时，12字节全零不能直接解释为损坏或未启动。
- 周期心跳只证明 P4 当前进度；不会把上一启动 reset=7 隐藏成正常复位。

## 当前实测（2026-11-10）

P4 三段烧写 hash 验证通过，运行 ELF 为
`526f21d8381fa09767b409baaa59ef2acb880b2a9412b2239e3aff9292763046`。
首次捕获因 Windows GBK 输出不能编码 UART 噪声而中止，未因此重刷；工具修正 UTF-8 后，
独立 INFO/心跳确认镜像已运行。首次启动 reset=7(WDT) 的来源未完成定位，
本轮连续采集期间未再次看到 task_wdt 或 panic；这不是长期稳定性结论。

原状态及一次 C5RESET 请求后，均卡初始化成功，CIS=`0092:6666`，5次12字节均完整且为0；
两轮清理成功。当前 headless 程序未运行 Hosted 握手，因此 Hosted/Wi-Fi/OTA 未验收。
后续已采集 C5 UART 完整启动：v1.2/4MB，出厂 network_adapter/Hosted2.12.9/IDF6.0.2，
SDIO stream初始化且统计持续至150秒；不再等待C5身份，暂不烧C5。见根目录docs/C5_SDIO_OTA_BRINGUP.md。

完整报告位于父工程忽略的 `test-results/new_devboard_*_20261110.json`，不提交备份或日志。

## 独立 Hosted 握手 profile（低速身份RPC已实测通过）

```powershell
python tools/p4_bringup.py --profile hosted --build
# 必须UART已人工切回P4；仅写P4，不能在C5路由时执行
python tools/p4_bringup.py --profile hosted --flash --port COM4 --capture 8
python tools/p4_bringup.py --profile hosted --port COM4 --capture 5 --command CONNECT --command VERIFY
```

使用build_hosted与该目录内的sdkconfig，不清raw/product构建缓存。
组件构造函数只初始化Hosted软件；开机不自动连接/复位C5。
CONNECT一次显式请求，由Hosted自身复位C5/初始化SDIO/等INIT并读取实时版本与芯片RPC；
1-bit400kHz，10秒INIT预算，不自动重连/循环复位，失败本次启动不再CONNECT。
VERIFY仅READY可执行，连续10次版本+芯片RPC，失败即停；不扫描Wi-Fi、不配置C5GPIO、不写Flash。
STATUS/心跳的状态明确标为CACHED，只有LIVE_RPC和本次终态通过才算实际通信验收。
固件拒绝PROBE/C5RESET等竞争owner命令；CLI同样按profile拒绝，不重复esp_hosted_init。
该profile不能视作运行时OTA或死固件恢复实现。

2026-11-10实机：上述Hosted镜像已烧P4/hash/运行身份通过，单次CONNECT收到INIT，
当前C5仍出厂2.12.9。首轮10组+追加100组+CONNECT一组，共111组/222次实时RPC全部成功；
末端uptime203298ms，无新panic/task_wdt。仅验证1-bit400kHz的芯片/版本查询，
不等于Wi-Fi/高速/OTA/回滚验收。C5未烧录，UART目前留在P4；无需再次切到C5来重复身份检查。
启动reset7仍未定位；详细报告为test-results/new_devboard_hosted_acceptance_20261110.json。

## 迁回自制板的边界

此程序没有 GPIO29 电源保持、产品 USB、屏幕或触摸，**不能作为最终自制板固件使用**。
开发板上跑通 C5 后，要把结果集成回原产品 P4 固件，保留既有电源保持、显示初始化、
180°旋转和 GPIO54/雷达共享保护。C5 Flash 的修改与这些 P4 板级配置分开管理。
