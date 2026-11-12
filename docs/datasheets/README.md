# 器件数据手册（Meoave 中控底板 V1.0）

本目录存放本项目实际用到的芯片与模组数据手册，按功能分六个子目录。
所有 PDF 均从原厂或原厂授权渠道下载，未做任何改动；校验值见文末「来源与校验」。

**说明**：目录里的文件是第三方版权资料，只作为本机调试参考。仓库是公开仓库且约定「第三方资料一律排除」，
因此 `docs/datasheets/*.pdf` 已加入 `.gitignore`（本 README 仍会进版本库）。若确实要入库，删掉那条忽略规则即可。

---

## 一、速查表

| 功能 | 器件 | 封装 / 接口 | 本板用法（引脚以 `firmware/p4_gesture/main/board_pins.h` 为准） | 文件 |
| --- | --- | --- | --- | --- |
| 主控模组 | **OSPTEK ESP32-P4C5-Core**（鱼鹰光电，网表器件名写 ESP32P4C6，同系列 88 pin 兼容） | 88 pin 邮票孔；P4 16MB Flash + 32MB PSRAM，内置 C5/C6 | U1。P4 走唯一 Type-C 的 USB HS OTG；C5 经内部 SDIO（GPIO14–19），C5 复位 = GPIO54 | `mcu/OSPTEK_ESP32-P4C5-Core_用户指南.pdf` |
| 应用处理器 | **ESP32-P4** | QFN；RISC-V 双核 HP + LP | 主控，跑 LVGL 与业务 | `mcu/ESP32-P4_数据手册_pre-release_v0.7.pdf` |
| 无线协处理器 | **ESP32-C5** | 模组内 | Wi-Fi 6 双频 / BLE，走 ESP-Hosted | `mcu/ESP32-C5_数据手册_v1.5.pdf` |
| 显示驱动 IC | **CO5300**（集创北方 Chipone） | COG，MIPI-DSI / QSPI / SPI | 屏 CN3（28P FPC），**1 lane DSI**，DPI 时钟 16 MHz；RST=GPIO12，TE=GPIO13，VCI_EN=GPIO20 | `display/CO5300_数据手册_V0.00_2023.pdf` |
| AMOLED 屏 | **Osptek AM200 系列 460×460** | 2.0"，AMOLED + 贴合电容触摸 | 460×460 方形画布，旋转 180° | `display/AM200Q460460LK_AMOLED_规格书.pdf` |
| 触摸 IC | **CST820**（上海海栎创 Hynitron） | 自电容，I²C | I2C0：SDA=GPIO8、SCL=GPIO9、RST=GPIO10、INT=GPIO11，7 bit 地址 **0x15** | `touch/CST820_数据手册_V1.2.pdf` |
| 音频 codec | **ES8389**（顺芯 Everest） | QFN-24 3×3 | I2C1：SDA=GPIO1、SCL=GPIO2；I2S：MCLK=3、BCLK=4、LRCK=5、DOUT=7、DIN=6；48 kHz/16 bit 从模式 | `audio/ES8389_数据手册.pdf` |
| 音频功放 | **NS4150B**（纳芯威 Nsiway） | MSOP-8，D 类 3 W | CTRL=GPIO24（高有效） | `audio/NS4150B_数据手册_V1.1.pdf` |
| 毫米波雷达 | **TI IWRL6432** | AOP 封装，UART / QSPI | UART1 115200：TX=GPIO21、RX=GPIO22；NRST=48、PWR_EN=27、SOP0=54、SOP1=43、HOST_REQ=23 | `radar/IWRL6432_数据手册.pdf` |
| LDO（codec 供电） | **HE9073**（赫尔 HEERMICR） | SOT-23-5，500 mA，0.3 µA Iq | U5，CE 由 GPIO26 控制，输出 CODEC_3V3 | `power/HE9073_数据手册_V1.1.pdf` |

## 二、调试时最容易踩的几处（都来自手册与实测）

1. **ES8389 没有 MICBIAS 输出**。手册 PIN OUT 里 24 脚只有 CCLK/CDATA/ADD、MCLK、SCLK、LRCK、DSDIN、ASDOUT、
   MIC1P/N、MIC2P/N、LOUTP/N、ROUTP/N、VMID、ADCVREF/DACVREF、PVDD/AVDD/DVDD 和地，没有 MICBIAS 引脚。
   板上 `ADC_MICBIAS12` 缺失会让 MEMS 麦 VDD 悬空，录音恒静音——这是本项目 ADC 通路不通的首要嫌疑。
2. **ES8389 的 I²C 地址由 AD0/AD1 strap 决定**（0x10/0x11/0x12/0x13），本板实测应答在 **0x12**，
   不是代码里原先假设的 0x10。扫不到 codec 时先看 `acs` 位图，不要直接怀疑焊接。
3. **CO5300 是带 GRAM 的驱动 IC**，但本板只有 1 lane DSI；改分辨率/帧率前先算带宽，不要按 4 lane 的经验调参。
   重复 reset/init 会破坏仍在工作的 DMA 与回调，启动阶段只初始化一次。
4. **GPIO54 是双向雷区**：既是雷达 SOP0，也是 C5 的 CHIP_PU（低有效）。拉低刷雷达 = 同时把 C5 按在复位里。
5. **NS4150B CTRL 是高有效**，且上电/掉电有 120 ms / 80 ms 的建立与关断时间，开关机爆音要按这个时序留裕量。

## 三、还缺的手册（下一轮补）

| 项目 | 现状 | 建议获取方式 |
| --- | --- | --- |
| 屏的 MIPI 版规格书 | 代码里屏型号是 **AM200MT460460FLS2**（28P FPC、1 lane DSI），本目录归档的是同系列 **AM200Q460460LK**（24P、QSPI、CO5300+CST820）规格书，两者面板同为 460×460 AMOLED，但接口与 FPC 定义不同 | 找鱼鹰光电 FAE 要 FLS2 的规格书；或从「IC + 屏」两份手册交叉核对 28P 脚序 |
| 充电 IC（U4）、3V3 buck（U10） | 只知道位置与作用，型号未在网表中固定 | 按丝印反查（芯查查/半导小芯），再补到 `power/` |
| MEMS 麦克风（U1/U2） | 板上两路模拟麦，型号未记录 | 查网表 BOM 或丝印 |
| 独立的离线语音芯片 | **本板没有**。语音通路是 ES8389（codec）+ NS4150B（功放） | 若要加 WT588F/SYN6288/XFS5152 之类离线语音模块，把型号告诉我，手册放 `audio/` |

## 四、来源与校验

下载日期：2026-11-12。SHA-256 取前 16 位，便于核对文件是否被替换。

| 文件 | 大小 | SHA-256 前缀 | 来源 |
| --- | --- | --- | --- |
| `mcu/OSPTEK_ESP32-P4C5-Core_用户指南.pdf` | 4.1 MB | `13620eed0d7404ad` | `https://yuyinglcd.com/strapi-uploads/ESP_32_P4_C5_Core_User_Guide_20260521_9fbca980ee.pdf` |
| `mcu/ESP32-P4_数据手册_pre-release_v0.7.pdf` | 1.5 MB | `fb4f3e91cc2ac519` | `https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf` |
| `mcu/ESP32-C5_数据手册_v1.5.pdf` | 820 KB | `e3cb7ad57b6fdf16` | `https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf` |
| `display/CO5300_数据手册_V0.00_2023.pdf` | 8.9 MB | `20161057fcb09a02` | 鱼鹰 `admin.osptek.com/uploads/CO_5300_Datasheet_V0_00_20230328_07edb82936.pdf`（与乐鑫 `dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf` 同一文件） |
| `display/AM200Q460460LK_AMOLED_规格书.pdf` | 3.1 MB | `ccca4a8051ef38a6` | 鱼鹰产品页 `yuyinglcd.com/products/2/5/512` |
| `touch/CST820_数据手册_V1.2.pdf` | 1.5 MB | `55087c404e7d7bc8` | 鱼鹰产品页同上（原厂海栎创版本 V1.2） |
| `audio/ES8389_数据手册.pdf` | 1.4 MB | `a67d768a33e62ed2` | `http://www.everest-semi.com/pdf/ES8389%20PB.pdf` |
| `audio/NS4150B_数据手册_V1.1.pdf` | 3.2 MB | `8f26ce641fd0ead9` | 纳芯威代理镜像（Mar.2021 V1.1，中文版） |
| `radar/IWRL6432_数据手册.pdf` | 2.1 MB | `ff9e987fd7bdcc92` | `https://www.ti.com/lit/ds/symlink/iwrl6432.pdf` |
| `power/HE9073_数据手册_V1.1.pdf` | 2.6 MB | `ca92c55814a59d05` | 赫尔半导体代理镜像（Ver1.1，Mar.2020） |

重下一份的示例（Git Bash）：

```bash
curl -sSL -o mcu/ESP32-C5_数据手册_v1.5.pdf \
  https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf
```

**版权**：所有手册版权归原厂（Chipone / Osptek / Hynitron / Everest / Nsiway / TI / Espressif / HEERMICR）所有，
仅作内部选型与调试参考，不得再分发或用于商业转售。
