# Meoave P4 目标计划

更新时间：2026-11-11

## 最新：免引脚维护（开发板10轮升级及故障恢复验收通过）

- 当前进度以`docs/C5_MAINTENANCE.md`为准。P4/C5正常及故障注入镜像已编译，173项离线回归通过。
- 新增`tools/c5_provision.py`（首次UART备份+有限区域安装）、`tools/c5_maintenance.py`（后续SDIO A/B升级/健康确认/失败回退/引脚租约测试）。
- C5默认GPIO2高阻，维护模式经SDIO限时切换；P4先释放共享GPIO6。正常OTA不依赖GPIO6。
- 用户确认切C5后，已用460800安装维护基线：整片4MB备份、三段写入hash/读回、完整4MB非目标区域保持比对通过。
  原厂ota_0/分区表/NVS/证书/标定在首次UART安装重启前读回中未改变；新应用实时RPC通过。
  之后已完成首轮SDIO升级，正常改写原厂ota_0应用槽；完整原厂备份仍留本地。
- 已修订Windows接收途中重配串口导致首包截短的问题：读命令前固定timeout、1KB停等校验，实测备份/烧录/整片读回通过。
- 用户已切回P4并安装单核维护固件。首轮+连续9轮1316640B SDIO升级/PENDING/确认VALID通过。
- 4096B中断、90秒未确认回退、启动前失败回退、SDIO失联95秒后单次EN回退、GPIO租约及最终正常VALID身份均实测通过。
- P4单独暖重连仍有累计计数不同步迹象，第96/47轮失败报告保留；上传前显式一次EN恢复旧VALID基线规避，
  新候选待确认阶段不复位C5，不宣称双核启动根因/任意暖重连已修复。
- 当前无屏开发板profile不能直接迁回自制板；产品电源保持/nativeUSB/音频仲裁集成仍待完成。
- Wi-Fi专项：P4已加入RAM凭据、真实扫描/DHCP/DNS/严格HTTPS测试入口，C5固件不变；187项离线测试通过。
  手机热点实测已通过DHCP/DNS及3轮完整HTTPS 200/证书校验；另一目标返回302，严格判据未通过。
  家用AP此前RSSI -92dBm/reason4失败仍保留，未定位根因；不代表长期稳定性或产品集成已完成。

## 1. 当前唯一工程

仓库只维护 `firmware/p4_gesture`，其他历史工程、下载包、临时验证目录不再作为项目入口。所有机器路径放在 `tools/config.toml`，所有常用动作通过 `tools/meoave.py` 执行。

## 2. 产品目标

### 首页

- 460×460 **方形** AMOLED，使用完整方形画布，不套圆屏遮罩或圆形安全区；显示方向固定旋转 180°。
- 纯黑背景、黑白简洁眼睛；眼睛是唯一主体。
- 支持眨眼、多表情和平滑方向输入；后续接入雷达目标方向。
- 点击首页进入 App 页。

### App 页

应用页是导航根层，不是 AI 页的子页面。保留三个同级真实入口：

1. AI：默认表盘，进入纯黑眼睛页；上电仍直接显示 AI，轻触打开应用页。
2. 设置：亮度、减少动态效果、Wi-Fi 等设备配置。
3. 呼吸：纯黑底、随真实波形展开与收拢的半透明圆形叠层；雷达尚未接入时静止等待，只在有效频率数据存在时显示次数／分钟。

### 最新视觉规范（2026-11-09，替代旧浅色方案）

#### 中文字体（2026-11-09，桌面预览与产品构建通过，未烧录）

- 用户先后反馈“字体不好看、看不出变化”“要圆圆的字体”，最终整套中文字库定为**资源圆体 Resource Han Rounded SC Medium**（SIL OFL 1.1）：思源黑体的全圆角版本，笔画端头是圆的，字面略饱，和 56px 圆角卡片、圆润图标同一套气质。
- 迭代过程留档：Noto Sans CJK（原）→ 寒蝉圆黑体 Medium（圆角太细，用户看不出差异）→ 荆南麦圆体（手写，用户要“圆”不要“手写”）→ 资源圆体 Medium（当前）。四套都保留为一键可切的字体集。
- 只换字形，不改规范：字号仍为 28px 正文 / 44px 名称与数值，ASCII 与符号集、触摸区、颜色、布局、眼睛与呼吸实现全部不动；16/20 两档历史字号一并重生成以保持同族，但界面仍未引用它们（属可后续删除的死重）。
- 生成链可复现：`firmware/p4_gesture/main/fonts/manifest.json` 记录字体家族、源地址、逐文件 SHA-256、逐档符号集、lv_font_conv 版本和字体包位置；`python tools/meoave.py fonts` 重新生成，`--set round|round_bold|chill|wenkai|yozai|maiyuan` 可整套切换。
- 字体源文件不进仓库：六套正文/标题字体加 Noto 对照共约 120MB，打包为 `meoave-fonts-v1.zip`（75MB）发布在仓库 `fonts-v1` Release，内含 `SHA256SUMS.txt`、OFL 1.1 全文与逐字体版权声明。六套字体**均为 OFL 1.1 且原样未修改**，允许随软件分发与商用，仅禁止单独出售字体文件；资源圆体上游只发 7z、单个 TTF 无稳定直链，这也是自建镜像的原因。
- `python tools/meoave.py fonts --fetch` 自动取包：直链失败会改走 GitHub API 资源地址，支持断点续传，解压前校验整包 SHA-256、解压后逐文件校验；已实测在一次 504 后在第二条通道完成。本地缓存 `build_ui_assets/cache/`，`fonts-src/` 仍不进 Git。
- 需要更胖更圆时用 `--set round_bold`（资源圆体 Bold）；更保守可用 `--set chill`；手写风可用 `maiyuan` / `wenkai` / `yozai`。切换后必须重跑桌面回归。
- 验证：真实 LVGL 桌面回归全部 PASS（含逐字 glyph 存在性与非 placeholder 断言）、45 项 Python、50 项 adapter、产品编译与最终 ELF UI 符号检查通过；快照与对照在 `firmware/p4_gesture/test-results/cute-font-v1/`，`before-after.png` 为麦圆 → 资源圆体同版本对照，`font-candidates.png` 为麦圆／圆体 Medium／圆体 Bold 三选，`zoom-28px.png` 为 28px 小字放大。
- 未烧录、未实屏验收：圆体在真实 AMOLED 上的笔画粗细与 28px 可读性仍需目视确认，不以桌面像素代替。

#### 呼吸全屏交互（2026-11-09，候选预览，未烧录）

- 用户指出顶部栏像手机应用：呼吸页移除返回箭头、标题及隐藏的整条点击栏；页面任何位置向右滑动返回 App 列表，拖动取消和减少动态仍沿用现有返回逻辑。
- 青蓝淡紫圆圈由 256px 画布调整为 320px，以完整 460×460 方屏中心为轴；有效呼吸频率位于圆圈下方，无有效数据时静止等待且不显示数字。
- 桌面模拟动画直接输出方形 LVGL 帧，不在设备画面上方拼接模拟标识条；模拟数据身份由文件名和预览说明标明。真实传感器与实屏效果仍未验证。
- 同套 LVGL 桌面渲染及手势回归：`firmware/p4_gesture/test-results/breath-fullscreen-v1/`，包括顶部、中心和底部起手右滑、误触、有效/无效/过期传感数据与减少动态；P4 产品构建和 UI 链接检查通过。字体沿用当前工作区已有改动，未回退其他工作；与旧快照相比，非呼吸页面也可能呈现已有字体差异。未烧录或实屏验收。

#### 呼吸圆圈视觉（2026-11-09，候选预览，未烧录）

- 用户希望呼吸主体沿用类似苹果呼吸应用的重叠圆圈，而不是上一候选的独立叶片。呼吸页面改成青蓝和淡紫色半透明圆形叠层，扩展、收拢由真实波形控制；等待状态保持静止且降低亮度。
- 保留追踪界面的无倒计时、无开始／暂停、无阶段提示和仅有效频率显示原则；此前候选中关于呼吸主体叶片的造型描述已被本节替代。后续全屏交互取代了呼吸页顶部箭头；App 页图标、首页和设置沿用当前工作区。
- 同套 LVGL 桌面输出：`firmware/p4_gesture/test-results/breath-circles-v1/`；`05-breathe-waiting.png` 为等待状态，`breathe-simulated.gif` 是桌面模拟输入，不代表真实雷达。桌面回归与 P4 产品构建通过，包含无数据静止、模拟波形变化、失效和返回手势；16 张非呼吸页面快照与前一候选一致。未烧录，实屏尚未验收。

#### 呼吸追踪界面（2026-11-09，候选预览，未烧录）

- 用户不希望呼吸功能变成计时训练：移除倒计时、完成状态、吸气／呼气阶段文字及开始／暂停；页面仅显示返回箭头和一朵花，有有效频率时才显示大字号 `数值 / min`。
- 产品 UI 默认追踪模式，`meo_ui_feed_respiration` 接收标准化波形、有效频率及时间戳；呼吸模型沿用现有平滑、有效性和 2 秒过期判断。无有效波形时花朵静止且颜色变暗，频率隐藏；失效或过期后不保留旧数值。没有注入任何演示数据，C5 和雷达硬件也未改动。
- 减少动态开启时花朵保持静态；有效的频率仍可显示。整条标题带可点击返回，页面右滑返回与原有滑动防误触保持。
- 同套 LVGL 桌面预览与回归在 `firmware/p4_gesture/test-results/breath-tracking-v1/`：`05-breathe-waiting.png` 为无数据状态；`09-simulated-valid-rate.png` 和 `breathe-simulated.gif` 来源于桌面测试注入的模拟数据，不代表实际接入雷达。
- 本轮真实 LVGL 桌面交互回归、45 项 Python、50 项 adapter、P4 产品构建与 UI 链接检查通过。测试覆盖无数据静止、波形变化、有效/无效频率、无效波形、2 秒过期、离页重进和减少动态；首页、App、设置及相关子页 16 张快照与上一候选逐字节一致。
- 造型重设计 App、设置和首页沿用上一个候选；设备仍运行已烧录的黑底彩色版。本版只是软件预览，未烧录或实屏验收；实际传感器协议、波形方向/缩放、信号有效性与频率来源仍待雷达接入和实测。

#### 造型重设计预览（2026-11-09，未烧录，等待选定）

- 用户反馈上一版“仍不好看”，本轮不再只换颜色，直接重做 App 图标造型、卡片与图形的主次、呼吸视觉；**先展示同套 LVGL 新旧对照，确认方向后再编译产品固件及烧录**。设备仍运行下方已交付的黑底彩色版，不把桌面预览当作已更新设备。
- App 仍是 158×158px 两个完整入口和两侧 24px 提示、三个真实应用循环。底板降低为极深色，保留细轮廓和轻柔按压态，让图形成为主体；原生图标从 96px 提升为 128px，统一厚实、圆润的视觉重量。AI 使用暖白笑眼、设置使用蓝色圆润齿轮、呼吸使用青色六瓣轮廓；首页黑白眼睛不变。
- 固定 44px 名称上移 20px（点击区 y=306、184×80px），收紧图文关系，仍只在吸附后淡入，不跟随滑动。名称触摸区不重叠；卡片、透明留白、露出条与滑动误触规则不变。
- 呼吸移除十二个透明圆片和黑色文字遮罩，改为一个 232×232px 被动原生绘制对象。六瓣使用不透明的平滑变宽曲线、同系三档青色，不使用纹理或额外矢量引擎；花瓣随原模型节律张开/收拢，减少动态时保持静态。
- 历史候选曾将阶段文字放入标题带并保留倒计时及开始／暂停；这些内容已被上方“呼吸追踪界面”替代。
- 真实 LVGL 桌面编译与交互回归、45 项 Python、50 项 adapter 通过；增加图形隐藏/恢复像素一致、阶段文字与花瓣不重叠、减少动态下整块花瓣像素不变断言。首页及七表情、设置与子页（含启用开关）共 13 张 PPM 与已烧录版本逐字节相同。
- 预览和软件报告：`firmware/p4_gesture/test-results/sculpted-bloom-v1/`。`before-after.png` 左侧为当前设备版本的 LVGL 像素、右侧为候选版本；二者均不是实屏照片。复跑对照可给 `tests/desktop/render_preview.py` 追加上一版输出目录作为第二个参数。
- 本轮**没有产品构建、烧录、复位或硬件操作**，不更改显示初始化、180° 旋转、电源保持、USB 下载、C5 或引脚。桌面主机耗时不代表面板帧率；产品构建、真实显示/色彩和触摸验收仍待选定后执行。

#### 黑底彩色修订（2026-11-09，优先于以下历史灰度规范）

- 用户明确要求的是**黑色背景，不是全界面黑白**。旧的“所有卡片、控件、图标只能黑灰白”限制作废；页面画布仍为纯黑，主要文字保持白色。
- App 维持「软糖双岛」尺寸、两张完整卡片和两侧露出、固定大字、滑动吸附与原有点击区；设置/AI/呼吸分别采用蓝/紫/青色图标与对应深色卡片，边缘仅变暗而不丢失语义颜色。
- 设置页保留列表原样，Wi-Fi 蓝色徽章、亮度琥珀色徽章/数值/滑块、辅助功能紫色徽章及启用时蓝色开关。未接入 Wi-Fi 的真实状态不变。
- 呼吸维持精简布局，花瓣采用青蓝/淡紫透明叠加，开始暂停按钮用青色；不添加文字、模式、说明或假传感数据。
- **AI 首页仍严格纯黑背景、白眼黑瞳，不加入彩色眼睛、光晕或嘴巴。** 不改字号、导航、触摸、电源、180°旋转、暖启动显示初始化或 USB 下载。
- 同套 LVGL 预览及回归输出：`firmware/p4_gesture/test-results/color-accents-v1/`。测试分别检查真实黑色画布、非首页彩色像素、首页全像素灰度、Wi-Fi 图标像素、开关启用颜色以及原有交互/字形；桌面、自动硬件运行和实屏验收分开记录。
- 本轮交付：真实 LVGL 桌面回归、45 项 Python、50 项 adapter、产品编译/最终 ELF UI 入口检查通过。循环克隆重定位及减少动态下语义颜色保持通过；首页及七种表情共 8 张 PPM 与上一版逐字节相同。
- 已经原生 USB 下载并校验三个镜像，自动启动及额外三次普通程序复位通过，无需按 SW1；运行 ELF SHA：`0d6db505a4bc2ad7ed9fd112beb8a4dd97a430ee4f1471626afeda7f61f78f08`。8 秒只读运行检查通过，眼睛页 GUI 推进、亮度 81%、减少动态关闭、180° 配置及电源保持正常。报告：`color_accents_hardware_20261109.json`、`color_accents_runtime_20261109.json`。**彩色版本的实屏色彩、显示与实际触摸仍为 PENDING_USER，不以自动结果替代目视确认。**

#### App「软糖双岛」V1（2026-11-09，本轮当前方案）

- 用户从多款概念中要求先落地一版，采用推荐的「软糖双岛」。只调整 App 导航；设置首页及 Wi-Fi、亮度、辅助功能子页恢复为上一版已交付列表，不保留未烧录的双组件设置实验。首页与呼吸实现不变。
- 460×460 方屏、纯黑底；中间两个完整的 158×158px 按钮，圆角 56px，中心距 190px；两侧各露出 24px，表示仍可横向浏览，不把边缘细条做成小型启动按钮。
- 仍只有设置 / AI / 呼吸三个真实应用，使用三组复用卡片形成循环预览；没有虚构第四个功能。默认上电显示 AI 眼睛，轻触后的 App 页首先完整展示 AI / 呼吸。
- 下方名称使用原生 44px 字体，184×80px 点击区域（左右不重叠）；名称为页面直属对象，不随图标滚动。开始拖动隐藏名称，吸附后更新并用 160ms 淡入；反向拖动立即取消旧淡入，避免旧名称闪回。两个完整卡片和对应文字留白都能进入应用。
- 继续使用原生 LVGL 横向跟手、惯性和吸附；循环重定位仅在吸附结束后发生。为规避当前 LVGL START/SCROLL_ONE 零距离边界造成的跨两项预测，只开放当前及相邻位置为吸附目标；不修改第三方 LVGL 源码。
- 无全卡缩放、模糊或额外合成层，仅轻量侧卡下沉/黑灰层次。减少动态效果关闭惯性、弹性与下沉，并立即呈现名称；不改硬件刷新调度，不宣称实屏达到 60fps。
- 保留 180° 显示/触摸映射、GPIO29 电源保持、原生 USB 下载、CPU-only 退出下载模式及暖启动花屏修复；不接触 C5、GPIO6/54 或 COM3。
- 同套 LVGL 源码预览与回归：`firmware/p4_gesture/test-results/soft-islands-v1/`。已检查两个完整项与两侧露出、循环双向越界、固定文字/淡入取消、旋转输入、防误启动、文字点击、减少动态和原有全页返回。设置首页 PPM 与已交付黑色基线逐字节一致。桌面/软件验证与新固件实屏验收分开记录。
- 最终软件验证：真实 LVGL 桌面回归、45 项 Python、50 项 adapter、产品编译/ELF UI 入口检查全部通过；设置首页及 Wi-Fi、亮度、辅助功能四个 PPM 与已交付黑色基线逐字节一致。为给左右文字触摸区留出间隙，最终名称区域使用 184×80px，并补充中间空隙不误触、透明文字留白可点击的回归。
- 最终版已通过原生 USB（COM10 → COM9 → COM10）下载/三个镜像校验、CPU-only 自动启动及额外三次程序复位，全程无需 SW1；运行 ELF SHA：`3984878304dbdb3df954f5d865f6f82d02ffd52ecd3b2ebf3067b68b23a7d3c8`。随后 8 秒只读运行检查通过，GUI 推进、亮度 81%、减少动态关闭及 180° 配置保持。报告：`firmware/p4_gesture/test-results/soft_islands_hardware_20261109.json`、`soft_islands_runtime_20261109.json`，源码指纹与软件汇总见 `soft-islands-v1/verification.json`。**本版 App 外观、物理触摸与滑动流畅度仍待用户实屏确认，不以 ready 或主机耗时代替。**

以下大型单卡轮播、旧浅色方案及历史交付记录仅保留作追溯；尺寸/布局与本节冲突时以本节为准。

- 历史灰度版曾限制卡片、控件、文字和图标只用黑灰白；**此配色限制已被上方“黑底彩色修订”作废**，仅纯黑页面背景保留。
- **禁止小字**：所有可见产品文字使用原生 28px 或 44px 字体，不用缩小字号塞内容。首页继续只保留黑白眼睛。
- 呼吸页删除模式选择、频率加减、经过时间、来源说明及工程状态，连隐藏的旧控件对象也不再创建；仅保留花瓣动画、中央 44px 阶段词、44px 剩余时间、112×84px 开始/暂停按钮。
- 准备、运行、暂停和完成使用同一个两分钟会话倒计时，阶段文字限“准备／吸气／呼气／暂停／完成”，不再让计时数值在阶段秒数和总时长之间切换含义。
- 雷达接入接口保留在模型/UI API 中，不提供不可用的模式入口；仅有效测量时显示带单位的数据，失效立即隐藏，不把引导节律显示为实测值。
- 保留整块标题带点击返回、全页右滑松手提交、手势隔离、减少动态效果，以及已验证的 180° 旋转、供电保持、原生 USB 和暖启动显示初始化。
- 同套 LVGL 预览与回归输出在 `firmware/p4_gesture/test-results/ui-desktop/black-theme-v1/`；桌面渲染、固件运行检查与用户实屏验收分别记录，不能互相代替。

本版交付记录：同套 LVGL 桌面回归、所有可见字形/图标像素检查、黑灰 RGB 像素断言、全局 28px 最小字号及呼吸 44px 字号/精简控件断言通过；45 项 Python 测试与 50 项 adapter 测试通过；产品固件编译及最终 ELF UI 符号检查通过。已通过原生 USB（COM9 下载 → COM10 运行）写入并校验三个镜像，额外 3 次程序复位全部通过，`pad_hold=true`、`power_ready=true`、`key_pressed=false`，无需人工按 SW1。运行 ELF SHA：`f6d3c3d73a302f746f146af18c370cfed74f159573baf5728afbfabd2001def4`。自动报告：`firmware/p4_gesture/test-results/black_theme_hardware_20261109.json`。**新黑色版的面板显示、字号可读性和实际手指交互仍待用户目视验收，不沿用旧版验收。**

烧录后 8 秒只读 GUI 观察通过：运行身份匹配、GUI tick 持续推进、boot_id 不变、180° 配置和供电保持正常；亮度 81%、减少动态关闭，雷达未接入。报告：`firmware/p4_gesture/test-results/black_theme_runtime_20261109.json`。软件检查汇总及源码指纹：`firmware/p4_gesture/test-results/ui-desktop/black-theme-v1/verification.json`。

交互不套用手机列表/小图标范式，而采用手表/陪伴设备式的大组件：

- App 页不显示标题、返回按钮，也不通过右滑返回 AI；AI 与设置、呼吸是同级应用，而不是它们的父页面。
- 应用页使用大型横向轮播圆角卡片：356×360px，圆角 44px，间距 16px；中间完整展示一个入口，两侧露出相邻卡片；顺序为设置 / AI / 呼吸，初始聚焦 AI。
- AI 卡片图标为两条粗圆头的白色微笑眼弧，黑底，不再使用两个带小瞳孔的竖椭圆；这仅是应用入口图标，首页眼睛与表情行为保持不变。
- 左右拖动跟手，松手单卡惯性吸附居中，带轻量视差和位置指示；不使用全卡缩放、模糊或额外合成层。点当前卡片进入，点侧边露出的卡片先居中，拖动不得误进入应用；退出应用后保留原聚焦项。
- 遵循既有 15ms LVGL 刷新调度，桌面测试同样用 15ms 调度并记录动画样本及主机处理耗时；这不代表实屏已达到 60fps，实际显示 FPS/拖影/触感需实屏验收。
- 开启减少动态效果后取消视差、边缘弹性与惯性，松手在下一次刷新直接吸附；不影响左右切换或点击。
- 设置页使用三块纵向的大型设置组件，统一 396×96px，图标、名称、状态对齐；不再因错误的圆屏假设收窄最后一行。
- 设置项名称与状态均使用原生 28px 中文字体，设置子页必要文字同样至少 28px；亮度子页数值使用 44px 数字字体，不通过缩放 20px 位图字体放大。保留现有触摸区域和控件位置。
- 子页返回只显示轻量箭头，移除常驻大圆底，但保留 64px 透明点击区；设置、呼吸和设置子页保留边缘右滑返回。
- 组件视觉尺寸与触摸区域分离，整块卡片均为点击目标；显示仍固定单次 180° 旋转，不更改硬件初始化及下载流程。

返回交互 V1（2026-11-09，已实现，待实屏验收）：

- 子页不再依赖小箭头：顶部 `396×80px` 标题带（x=32..428、y=32..112）整块可点，箭头视觉尺寸提升至 40px；标题和两侧留白均可返回。
- 主要返回方式改为全页面大范围右滑：从设置、网络、亮度、辅助功能、呼吸页面的非滑块/开关区域起手，向右拖动超过 80px 且横向主导后松手返回；拖动过程中当前页跟手右移，露出真实父页面预览，松手提交。
- 右滑不足 80px、向左、明显纵向/斜向拖动都会取消并回到原位；未松手前不切页；返回提交会吞掉本次释放事件，避免点穿到父页面。呼吸返回提交时停止当前练习并清理波形。
- 滑块、开关和 App 页横向轮播优先保留自身手势；页面切换/返回动画期间的新触摸被吞掉；AI 首页和 App 轮播页仍无返回手势。
- 减少动态效果开启时仍使用同一返回判定和触摸隔离，但松手后直接完成，不显示跟手动画。
- 桌面真实 LVGL 回归已通过，包含旧边缘路径迁移到“松手提交”、拖动取消、滑块隔离、横向轮播不受影响、旋转坐标输入、无点穿和完整标题点击；预览输出在 `firmware/p4_gesture/test-results/ui-back-v2/`。产品构建正在执行，尚未烧录、尚未实屏验收。

2026-11-08 卡片轮播交付进度：

- 同套 LVGL 桌面渲染已生成完整方屏静态图和 `carousel-square.webp`（另保留通用动画文件名），包含跟手拖动、吸附和端点回弹。预览直接使用 LVGL 像素，不作圆形裁切。
- 桌面交互回归、50 项 adapter 测试、45 项 Python 测试及 C model 测试通过；子 AI 只读复核的滚动误触、慢速边缘返回、性能计时三项修复通过。
- 修复产品 UI 依赖可选 widgets demo 启动的缺陷：在原有硬件初始化完成并持有 LVGL 锁后显式调用 `meoave_app_create()`，不再依赖链接器替换 demo。
- 产品固件构建成功，最终 ELF 已确认包含 `meoave_app_create`、`meo_ui_create` 和 `carousel_scroll`。一键 build 新增最终 ELF 的 UI 符号检查，防止构建成功却剔除了 UI。
- **已完成 P4 产品固件烧录和自动硬件验收；用户随后确认本版实屏检查“全部正常”：方向、卡片滑动/吸附、笑眼图标、设置/呼吸入口、开始暂停及子页返回。实屏 FPS 未定量测量，桌面调度与主机耗时不代表面板 FPS。**
- 方屏修订已通过同套 LVGL 桌面交互回归（包括实际方屏四角、旧圆形边界外像素、笑眼路径及侧卡点击/间隙拖动断言）和产品固件构建；硬件初始化、触摸引脚、电源保持与原生 USB 流程未改动。

2026-11-08 烧录自动验收：

- bootloader / 分区表 / 应用三个镜像写入并分别校验通过；运行 ELF SHA 为 `9dab4f2f3ecc89a2a2481ba1bde0af57ea2f42de82a82041b6aeb0b2876e3346`。
- 原生 USB 应用 COM10 → ROM COM9 → 应用 COM10，一次完整下载及额外三次程序复位全部通过；各次 boot_id 更新、电源保持有效、SW1 松开。
- 触摸控制器 ID `0xB7`，各次实际驱动读取持续增长，通信错误为 0；这不等于实际手指交互已通过。
- 三次复位后的 UI 记录均保留亮度 81%、减少动态效果开启；已提醒用户实屏验收动画前先关闭减少动态效果。
- 8 秒 UI 运行检查通过：GUI tick 持续增长、单次 180° 方向配置、运行固件身份与电源保持均正常；物理显示方向仍需目视。
- 额外 60 秒只读观察中 UI 持续运行、boot_id 不变、故障记录无异常、触摸驱动错误为 0；仅观测到 eyes 页且没有实际按压样本，**不能据此判定卡片滑动和手指点击实屏验收通过**。观察报告为 `square_cards_touch_observation_20261108.json`。
- 首次全量重编译有依赖编译进程异常退出，未进入烧录；限制并行到 8 后构建完成。构建工具默认限制并行数（可在 `[build].jobs` 调整），并自动补齐可发现的 ROM ELF 环境路径。
- 自动报告：`firmware/p4_gesture/test-results/square_cards_hardware_20261108.json`、`square_cards_ui_runtime_20261108.json`；C5 未烧写，Wi-Fi/雷达不计入本轮 UI 验收。
- 后续用户实屏验收回执为“全部正常”，补齐自动观察不能证明的显示与手指交互检查；此回执仅适用于上述已烧录的方屏卡片版本，不代表认可卡片尺寸或完成新的交互方案。

后续交互设计讨论（尚未实施或烧录）：用户反馈卡片仍偏大，倾向让 AI 直接作为主页面。当前建议为取消应用选择页，改为 AI 主屏与呼吸左右切换、下拉快捷设置；该方案与已验收的卡片版本分开记录，不能将本次回执用于新方案的验收。

设置文字可读性修订：针对用户实屏反馈“设置里面的字太小”，已将设置项名称/状态及设置子页正文从 20px 提升至 28px，亮度数值提升至 44px；同时为亮度加减符号使用正确的符号字体。桌面测试新增所有设置页面的字号、单行显示和父容器边界断言，覆盖 100% 亮度与辅助功能状态；同套 LVGL 渲染已检查，本轮保留原有首页与导航，不提前实施上述交互重构。新字号的实屏可读性需要单独确认，不沿用旧版验收回执。

- 本版已通过产品编译/ELF UI 符号检查、同套 LVGL 桌面回归、45 项 Python 测试与 50 项 adapter 测试。
- 子 AI 审查发现原有状态文案“轻柔”的“柔”缺字，已将状态改为字库支持的“减少／标准”，并新增逐字检查：使用同一 LVGL 的 UTF-8 解码器，断言每个设置文字和符号的 glyph 存在且不是 placeholder。修复后桌面测试和子 AI 复查通过。
- 最终版已通过原生 USB 写入并校验三个镜像，额外一次程序复位成功，SW1 松开、电源保持及实际触摸驱动读取正常。报告：`firmware/p4_gesture/test-results/settings_font_final_hardware_20261108.json`；运行 ELF SHA 为 `cd56c1f858b5e1ebb4a0e7fe8b0c20a1da00391c6d7aa02809f92c322e83788a`。
- 最终版烧录后 8 秒 GUI 运行检查通过，亮度 81%、减少动态效果关闭的设置保持正常。报告：`firmware/p4_gesture/test-results/settings_font_final_runtime_20261108.json`。上述自动结果不替代用户对新字号大小的目视确认。

图标显示修订（2026-11-08）：用户反馈“很多图标不显示”，并要求自动检查。当前已确认运行的旧版固件与构建 ELF 一致；子 AI 直接核对最终 ELF 中 Montserrat26 的 cmap/bitmap，61 个非 DUMMY 的 LV_SYMBOL 均有非空字形，因此**没有证据将用户的所有缺图标归因于漏字库**。

- 自动排查已复现 Wi-Fi 子页 `transform_scale=450` 图标的局部重绘不一致：同 tick 隐藏再显示后 635 个像素与初始帧不同；这是软件渲染缺陷证据，不等于已复现用户实屏全部缺失。
- 所有文本符号图标（返回/行尾箭头、Wi-Fi、显示亮度、辅助功能、加减、播放/暂停/重播）统一改成 LVGL 原生线段/圆弧/三角形，使用原生 24/28/32/64px 几何，不依赖私用区字形或变换图层。图标不可点击，事件仍交给原有大触摸容器；播放态只更新图标类型，不逐帧分配对象。
- 保持 28px 设置正文、44px 亮度值、现有导航与卡片尺寸。未改变显示初始化、触摸引脚、供电保持、原生 USB 路径及单次 180° 旋转；不扩展 Wi-Fi/C5。
- 桌面配置 `LV_USE_FLOAT=0` 对齐产品的坐标/绘制描述符布局；不在无 OS 的桌面环境伪装测试产品的双 FreeRTOS 绘制单元。
- 自动回归：所有可见页面/动态文案逐字检查；原生图标逐个实际 RGB888 像素差分，隐藏有差异、恢复逐像素一致，检查父容器范围和无缩放；覆盖设置各子页、呼吸准备/运行/暂停/完成重播/雷达隐藏态，并检查 Wi-Fi 底部圆点与 App 所选卡片图形。严格区分此桌面检查和实屏识别。
- 同套 LVGL 桌面回归、产品增量构建/最终 ELF UI 入口检查、45 项 Python 与 50 项 adapter 回归均通过。预览在 `firmware/p4_gesture/test-results/ui-desktop/interaction-preview.png`；暂停与完成状态另有 `28-breathe-paused.png`、`29-breathe-complete.png`。
- 最终版本已通过原生 USB 写入/三个镜像校验、一次额外程序复位、运行 ELF SHA 对照、SW1 松开与供电保持检查；无需人工开机按键。ELF SHA：`cbdc5ca8d6566f0a025b5e01ecbf841004503eb0713075d6e05d8bd20cd58136`。报告：`firmware/p4_gesture/test-results/icons_native_hardware_20261108.json`。
- 烧录后 8 秒只读运行检查通过，亮度 81%、减少动态关闭、180° 配置保持，GUI tick 正常推进。报告：`firmware/p4_gesture/test-results/icons_native_runtime_20261108.json`；桌面像素检查报告：`firmware/p4_gesture/test-results/icons_native_software_20261108.json`。
- 子 AI 独立审查发现第一版原生 Wi-Fi 圆点使用零长度线段、会被 SW 渲染跳过；已改为实心圆弧，补充圆点中心像素断言，最终回归和复查通过。
- 尚无新图标的实屏确认。没有屏幕照片或摄像头输入，不能自动确认物理面板是否真正显示，不能把桌面像素检查或设备 ready 状态算作实屏验收。当前桌面像素检查也不是设备帧缓冲抓取。

自动复跑图标与交互检查（仓库根目录，无需触摸）：

```powershell
cmake --build firmware/p4_gesture/tests/desktop/build -j 8
firmware/p4_gesture/tests/desktop/build/meoave_visual_test.exe firmware/p4_gesture/test-results/ui-desktop
```


启动后黄色花屏专项诊断（2026-11-08，**尚未认定修复**）：

- 用户现已确认：烧录/软件重启后一直黄色花屏，只有硬件复位才能恢复，属于暖启动显示失败，不是几秒的启动残影。旧图标固件 ELF 确已运行，不能解释成“未更新程序”；旧有 GUI tick / draw_ok 持续增长不能替代显示验收。
- 新增两条显式诊断命令：`DISPLAY FRAME` 在 GUI 锁内只读本 profile 的真实单个 460×460 RGB888 DPI fb0，统计哈希/黑白/彩色/黄色像素及非黑区域；`DISPLAY REDRAW` 仅请求当前屏完整失效并立即刷新。USB 回调只置请求位，实际工作在应用任务中，不改 DCS 初始化表、时序、引脚、供电或下载机制。
- 诊断固件 ELF SHA：`a6eaeac1f63b5d84f6a8d77ee52f333a9afa341e7f98ce12c8374675014b8053`。已通过原生 USB 三镜像校验、CPU-only 退出 ROM、额外一次正常程序复位，SW1 松开且供电保持。
- 重刷后实测 634,800 字节 fb0：黑色约20.3万像素、白色约0.8万像素，`colored=0 yellow=0`，多次哈希/位置随眼睛动画变化。额外程序复位后结果相同，完整 REDRAW 也已执行。这仅证明 CPU 看到的 scanout 后备内存中没有黄色花屏，不证明 DMA 实际取数或物理面板出图正常。
- DSI 第一次只读状态采样 `int0=0x2000`，紧接第二次采样已为0，`int1`两次均为0；不能将历史错误单次读数直接定为持续花屏根因。尚未统计实际帧传输完成、bridge underrun，也没有电压测量。
- 只读审查发现当前 CO5300 依赖的删除函数重复删除底层 panel，因此本轮**不采用运行中 delete/recreate 或盲目重复 reset/init**；这些动作可能破坏仍工作的 DMA/回调。也没有证据支持任意修改 DSI/PSRAM 时钟或归咎 NVS。
- 新增诊断接口通过产品构建及实际读回检查；45 项 Python 回归通过。子 AI 未发现越界/锁泄漏/生命周期问题；命令请求会按服务顺序合并，因此自动工具必须逐条等待 FRAME/REDRAW 回执，不能同一批发送后假设 FIFO 执行。本轮采样均逐条间隔进行。
- 报告：`firmware/p4_gesture/test-results/display_corruption_before_20261108.json`、`display_frame_diag_hardware_20261108.json`、`display_frame_diag_runtime_20261108.json`、`display_frame_diag_restart_20261108.json`。用户补齐持续故障回执，暖启动物理显示为 `FAIL`，不能继续以软件正常标为通过。

暖启动修复与验收（2026-11-08 实施，2026-11-09 烧录启动及单次程序复位实屏通过）：

- 本地 IDF 6.1 中，`mipi_dsi_ll_reset_register()` 只复位 bridge；HAL 初始化直接将 host `shutdownz` 写 1。正常 `esp_restart()` 虽先 abort DW_GDMA，却未重置 DSI。已发现冷启动与暖启动的起始状态可能不同，但尚不能将此单独确认为花屏根因。
- 新增仅启动阶段调用一次的 `fls2_display_prepare_startup()`：取得 PHY LDO 后、创建任何 DSI/DPI/LVGL 对象前，先锁存 GPIO12 为低并配置输出；在 RCC 原子区打开 DSI 寄存器访问时钟、关闭残留 DPI 时钟；host/PHY 置关闭、读回检查并等待 10ms，然后完整走原 SDK 的 bus 创建、CO5300 reset/init（5/10/120ms）和原显示管线。
- 此步骤不适用于运行中的驱动，不执行 delete/recreate、共享 DMA 硬复位、全芯片复位、VCI 电源循环或时钟降频。不改变 UI、触摸、180°旋转、GPIO29、电源保持或原生 USB 下载机制；不扩展到任意绕过 IDF 清理的裸 CPU reset。
- `DISPLAY HW` 新增 `FLS2:DISPLAY_START`，提供继承的 host/PHY 值与关闭读回；该记录只证明软件执行和寄存器状态，不是面板接收证明。主机 mock 时序测试与实际冷/暖屏幕验收分别记录。
- 产品构建与 UI ELF 入口检查通过，45 项 Python、50 项 adapter 回归通过；子 AI 已复核实际补丁，未发现阻断本次受控实验的问题。
- 已经一次原生 USB 烧写三个镜像并校验，CPU-only 退出 ROM，运行 SHA 为 `314954d7b1254a291f64b30ed98275403022b38a9e81bb41f83c69cd03f87186`；SW1 松开、电源保持正常。本阶段刻意不追加程序复位、不做硬件复位，先等用户确认烧录后首次启动的实屏。
- 设备启动快照证实继承值 `host_before=0x1 phy_before=0xF`，补丁关闭读回 `host_off=0 phy_off=0xE`，说明这次暖启动确有上次启用状态且新的关闭步骤执行。运行采样 `int0=0 int1=0`、眼睛页 GUI 推进，仍然不等于实屏恢复，也不能据此单独断言 host 位就是根因。
- 报告：`firmware/p4_gesture/test-results/display_warm_start_hardware_20261108.json`、`display_warm_start_snapshot_20261108.json`。该自动采样阶段的实屏原为 `PENDING_USER`，已由下述用户回执补齐；旧版本的实屏 `FAIL` 历史结论保留，不以新版本的自动软件通过覆盖。
- 新增 `firmware/p4_gesture/tests/display_startup/` 编译真实启动 helper，25 项主机 mock 时序/读回/错误路径测试全部通过（包括成功后拒绝再次执行）。它不模拟完整 DSI/面板，只检查受控调用和寄存器位操作。另完成烧录后 8 秒运行检查：无额外复位，GUI 推进、方向配置、原设置与电源保持正常，报告 `display_warm_start_runtime_20261108.json`。
- 2026-11-09 用户对本次烧录后自动启动的实屏明确回执：“正常显示，黄色花屏消失”。因此本版 **退出 ROM 后首次启动实屏 PASS（用户确认）**；该回执不自动覆盖后续正常 `esp_restart()` 或连续多次复位。下一阶段单独做一次普通程序复位，不重刷、不做硬件复位，再等待该阶段实屏确认。
- 随后完成一次普通 `esp_restart()`，没有重新烧写、没有硬件复位、无需按开机键；boot_id 从 `76fb13d9` 变为 `c4bca3d8`，运行 SHA 一致，GPIO29 持续保持、SW1 松开、USB 恢复、触摸实际驱动读取推进且通信错误为 0。用户对这一次复位再次明确回执：“正常显示，无黄色花屏”。因此 **单次普通程序复位实屏 PASS（用户确认）**。
- 复位后追加 8 秒只读运行检查通过，boot_id 未再变化、GUI 持续推进，亮度 81%、减少动态关闭和单次 180° 配置保持。报告：`firmware/p4_gesture/test-results/display_warm_start_restart_20261109.json`、`display_warm_start_restart_runtime_20261109.json`；人工视觉结论与自动软件结果单独保存，未将本次观察扩大为触摸映射、全部子页或多次循环长稳验收。
- 本轮结论：**暖启动黄色花屏修复在“一次完整下载自动启动 + 一次普通程序复位”范围内完成实屏验收**，原有免按键下载/复位保留。证据支持新增启动前的 host/PHY 关闭与屏复位顺序有效，但不单独证明某一寄存器位就是全部根因，也不宣称所有复位来源或长期可靠性均已覆盖。

## 3. 硬件与 Wi-Fi 方案

2026-11-09 更新：用户手动切换 C5 boot 后，CMD5 实测已从 NO_RESPONSE 变为 SDIO_R4，
OCR=0x2100ff00，2 functions，探测前后 P4 boot_id 一致、无 fault。新增独立 programmer
profile 和 `tools/meoave.py c5-program`，用于不操作 GPIO6/54 的首次 SDIO 烧录。
后续升级采用 Hosted 双槽 OTA；GPIO6/C5 GPIO2 可以考虑带所有权确认的分时，
首次镜像继续保持 C5 GPIO2 高阻。执行方式、边界与验收状态见
`docs/C5_UPDATES_AND_GPIO6.md`；下列 2026-11-08 的 NO_RESPONSE 是历史记录。

- 主控：鱼鹰光电 OSPTEK P4C5。
- Wi-Fi 控制/数据：ESP-Hosted over 4-bit SDIO。
- P4 SDIO：D0/D1/D2/D3 = GPIO14/15/16/17，CLK = GPIO18，CMD = GPIO19。
- GPIO6：音频 I2S DOUT，禁止被 Wi-Fi 使用。
- GPIO54：C5 CHIP_PU 与雷达 SOP0 复用；未完成安全时序前禁止自动拉低、重试复位或低功耗唤醒。

Hosted 独立 profile 已完成编译级接入，但以下项目仍未完成：

- 读取实际 C5 Hosted 从机固件版本
- 无误伤雷达的 SDIO 握手
- Wi-Fi 扫描、凭据输入、连接、IP 和断线重连
- 设置页真实 Wi-Fi 状态

因此不能把 Hosted 编译成功当成联网成功，也不能在未验证前烧写 C5。

C5 诊断入口固定为 `python tools/meoave.py wifi`：只发一次 CMD5(arg=0)，不复位、不烧写 C5；有 R4 应答也只代表 SDIO 电平/协议层有响应，不代表 Hosted 握手、芯片型号或 Wi-Fi 可用。手动把 C5 复位进下载模式后改用 `--no-reset`：它跳过"P4 必须先做过程序复位"的前置条件（整板断电上电后 P4 的 reset_reason 不会是 3），但仍要求探测前后 `boot_id` 不变、`cleanup/blocked` 为 0，并在报告里记 `clean_start=false`、`c5_reset=never`。引脚级诊断另有 `WIFI PINS <token>`（只读电平）与 `WIFI PIN54 <token> HIGH`（仅释放复位，代码层面拒绝 LOW）。

2026-11-08 实测结论：product 固件从不配置 GPIO54，默认态 `pin54=1`（模组侧上拉维持），显式驱高无争用；两次 CMD5 均 `NO_RESPONSE`，且 1 ms 就返回硬件响应超时（软件上限 250 ms），说明 P4 的 CLK 在跑、命令帧已发出。因此**复位不是原因**，故障在 C5 一侧没有呈现 SDIO 从机。模组出厂带固件，但 CMD5 连 R4 都没有，也说明它**不在 SDIO 下载模式**——即固件已启动、只是没拉起 SDIO slave（出厂镜像不含 ESP-Hosted 从机），其次要排除模组变体/引脚映射不符。详见 `firmware/p4_gesture/test-results/c5_no_response_report.md`。

2026-11-08 烧录路径核查（结论：V1.0 **没有引出 C5 的 UART/BOOT，出厂态无法重烧**）：模组手册 3.2 节明确 4 脚 = C5_U0RXD、5 脚 = C5_U0TXD（C5 烧录脚）、6 脚 = C5_IO28（下载 strap）；而本板网表中 U1.4–U1.14 全部为 no-connect，板上唯一 Type-C 只接 P4 的 USB OTG（U1.48/49），没有 USB-UART 桥。模组引脚表从 GPIO13 直接跳到 GPIO20，GPIO14–19 未引出——这六根就是模组内部的 P4↔C5 SDIO，所以板上没有 SDIO 网络属正常。**最短补救路径（需 3 个 strap 同时拉低，不是 1 根线）**：ESP32-C5 Datasheet v1.5 表 3-3 规定，SDIO 下载只存在于 Joint Download Boot 1，要求 GPIO26=GPIO27=GPIO28=0；只拉低 IO28 会命中 Joint Download Boot 0（仅 USB-Serial-JTAG/UART，本板均未引出），等于无效。因此需把模组 6/7/8 三脚连片接地（相邻脚），保持低电平后整板断电上电，C5 即进入 SDIO 下载（无需 GPIO54 复位脉冲，天然避开雷达 SOP0）；再由 P4 用 esp-serial-flasher v2.0.0 的 `esp32_sdio_port` 烧写，该端口单个 `boot_pin` 必须同时驱动这三根 strap。零改动验证法：三脚接地后整板断电重新上电，再跑 `python tools/meoave.py wifi --no-reset`，返回 R4 即证明芯片、走线、下载通路三者成立。详见 `firmware/p4_gesture/test-results/c5_programming_path.md` 第 3.5 节。

2026-11-08 03:35 模组内部接线已由厂家图确认（核心板手册 p10 "Pin mapping between ESP32C5HF4 and ESP32P4"）：C5 的原生 SDIO 脚 GPIO8/7/14/13/9/10 分别接到 P4 的 14/15/16/17/18/19，与固件映射完全一致，**不需要再做引脚组合扫描**；另有两根非 SDIO 线：C5 GPIO2 ↔ P4 GPIO6、C5 CHIP_PU ↔ P4 GPIO54。推论一：P4 无法把 C5 切进下载模式，因为 C5 的 strap（GPIO26/27/28）只在模组 6/7/8 脚上；推论二：本板把 P4 GPIO6 用作 ES8389 的 I2S DOUT，等于 C5 的 IO2 与音频数据线共用，V1.1 需串 0Ω 或换脚。同轮实测：给探针加上 CMD0 前置后仍为 `NO_RESPONSE`（`idle=0`，证明命令已按时钟推出），C5 IO2 三态测量为高阻悬空（无上拉、未被 C5 驱动），故 C5 既不在 SDIO 下载模式、也没跑 SDIO 从机，P4 侧已无软件手段。

## 4. 自动化开发流程

目标是一条命令完成可重复的离线检查：

```powershell
python tools/meoave.py doctor
python tools/meoave.py build
python tools/meoave.py build --profile hosted
python tools/meoave.py test
python tools/meoave.py all --hosted
python tools/meoave.py wifi
python tools/meoave.py c5
```

C5 从机固件是独立工程 `firmware/c5_hosted`（esp-hosted-mcu v2.12.13，与 P4 侧 host 版本一致），
由 `python tools/meoave.py c5` 编译并自动合成单文件镜像 `build_c5/c5_hosted_slave_merged.bin`。
该工程对上游只有一处功能改动：`main/c5_io2_guard.c` 把 C5 GPIO2 永久钉成高阻（输入、无上下拉），
因为模组内部把它接到 P4 GPIO6，而本板那颗脚是 ES8389 的 I2S DOUT。改动的完整清单见
`firmware/c5_hosted/UPSTREAM.md`。

硬件验收：

```powershell
python tools/meoave.py hardware --build --cycles 1 --restarts 3
```

验收必须满足：

- 只识别主板原生 USB VID/PID，不依赖固定 COM 号。
- 烧写后校验镜像、程序复位、USB 恢复、运行 ELF SHA 和电源保持。
- 复位失败不得自动盲目重刷。
- 任何串口状态都不能替代实屏、触摸和真实 Wi-Fi 验收。

## 5. 工具系统原则

- `tools/config.example.toml` 是可提交模板；`tools/config.toml` 是本机私有配置。
- 优先使用配置路径，其次使用 `IDF_PATH`、`PATH` 和当前 Python 环境自动发现。
- 编译产物、日志、测试报告、ESP-IDF 生成目录不进入 Git。
- 清理命令只允许删除配置的两个构建目录，拒绝越界路径。

## 6. 完成定义

本计划只有在以下全部完成后才算完成：

1. 产品 UI 在实屏上方向、触摸、动画和返回逻辑验收通过。
2. 原生 USB 下载/复位连续回归不需要再次按开机按钮。
3. GPIO6 音频不受 Wi-Fi 初始化影响，GPIO54 共享时序经过实机验证。
4. C5 Hosted SDIO 实际握手、扫描、连接、IP 和断线恢复完成。
5. 一键工具在干净环境中能够完成 doctor、build、test，并能明确区分软件通过和实机未验收。

## 7. 2026-11-09 进展记录（C5 首次烧录）

- 已写入 C5 四个区域共 1 341 488 B 并逐段 MD5 通过（`state=DONE`、`verified=1`、
  `chip=6`、4 MB Flash、`ota_0/ota_1` 各 1.875 MB）。
- 修掉 `tools/c5_program.py` 解析 `VERIFY_MD5` 失败、`esp_hosted` 低有效复位序列
  把 C5 按在复位里两个缺陷；`sdkconfig.defaults*` 内容已纳入构建签名。
- C5 与 P4 源码两侧均已开启 `CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER`；但已成功烧录的
  C5 payload 早于该选项，后续 GPIO 版烧录失败，不能宣称远端 GPIO RPC 已部署/验收。
- 未完成：C5 Hosted 应用运行与握手。此前 CMD3/CCCR 失败不能直接证明从机复位，
  后续独立实测见下一节。Wi-Fi 扫描/连接仍未验收。

## 8. 2026-11-09 C5 自动化测试

入口：`python tools/meoave.py c5-test --build --install`。
只增量编译、安装 P4 一次；每组独立程序复位，配置 SDIO 位宽/速率，不重复编译，
不写 C5 Flash、不驱动 GPIO6。每组显式复位 C5 一次（共享 GPIO54，要求雷达未运行），
卡初始化一次，最多等待 4 秒 Hosted INIT；超时不自动再复位。

本轮完成六组：

| 配置 | 卡初始化 | Hosted INIT |
| --- | --- | --- |
| 1-bit / 400 kHz | 通过，无 CRC 错误记录 | 4 秒窗口内未收到 |
| 1-bit / 5 MHz | 通过，无 CRC 错误记录 | 4 秒窗口内未收到 |
| 4-bit / 400 kHz | 通过，无 CRC 错误记录 | 4 秒窗口内未收到 |
| 4-bit / 5 MHz | 通过，无 CRC 错误记录 | 4 秒窗口内未收到 |
| 4-bit / 20 MHz | 通过，无 CRC 错误记录 | 4 秒窗口内未收到 |
| 4-bit / 40 MHz | CRC 错误 0x109，未通过 | 未进入等待阶段 |

结论：降速不能单独解决 Hosted 握手问题；40 MHz 本轮还存在卡初始化 CRC 错误。
卡/功能初始化不等于 Hosted 应用运行，更不等于已联网或可 OTA，不能据此判定 C5 损坏。
首次下载用的模组 6/7/8 脚接地在当时待确认；2026-11-10 用户已确认全部解除。
不能继续把外部短接未拆作为已知原因，也不从上述日志猜测实际启动模式。

已部署保护：Hosted 占有 SDMMC 时拒绝旧 PINS/PROBE/PIN54 命令；连接失败正确返回，
不再继续 Wi-Fi 初始化引发第二轮重连；型号/版本 RPC 必须成功才能设置 READY。
P4 每组 USB/供电保持/触摸进度/故障记录检查通过，完成后已复位到默认 IDLE；实屏未目视复验。
Python 回归 69 项通过，Hosted 增量编译通过。报告：
`firmware/p4_gesture/test-results/c5_automated_20261109.json`（本地忽略，不入 Git）。

## 9. 2026-11-10 解除下载接地后的对照测试

- 用户明确确认模组 6/7/8 脚外部接地已全部断开。先以 1-bit/400 kHz 复测，
  卡初始化通过、无 CRC/SDIO 命令超时记录，仍未收到 Hosted INIT。
- 增量构建并仅更新 P4 诊断固件一次；不写 C5 Flash、不配置 GPIO6、不依赖 COM3。
  当前 P4 ELF SHA：`dbceed16431cf148db35f02ca5fc5d6474db13cb782b39ebd8a708f40a50255c`。
- 在同一固件上分别比较正常中断与 100 ms 接收轮询；均为 1-bit/400 kHz，
  INIT 名义观察窗口延长至 20 秒。每组一次 C5 复位/卡初始化，未追加握手重试。
- 两组卡/功能初始化通过，打开数据通路的 CMD52 写返回成功，无 CRC/SDIO 命令超时记录。
  每组 20 次只读 CMD52 快照全部成功；pending/token/raw/st/len 始终为 0，
  ien=0x03、ena=0x00800040。两组都未收到 INIT、未取得真实从机型号/版本，Wi-Fi 未启动。
- 结论：单纯延长等待或绕过 DAT1 中断等待未解决；尚不能确认 C5 Hosted 应用已运行，
  不能仅凭这些寄存器判定 ROM 模式、复位循环或芯片损坏。已核对本地官方 C5 IDF
  寄存器偏移、从机缓冲装载和 OPEN_DATA_PATH 通知代码，未据此发现可直接归因的新缺陷。
  下一步需要独立的 C5 启动证据（优先从机 UART 启动日志），而不是继续盲扫速率或反复刷写。
- 两组测试后均经程序复位恢复 P4 默认 IDLE/diag=0；USB、电源保持、触摸读取进度和
  FAULT valid=0 检查通过，无需手按开机键。本轮未重新目视验收显示，也未验收联网/OTA/GPIO 分时。

原始报告（本地忽略）：`firmware/p4_gesture/test-results/c5_unstrapped_20261110.json`、
`c5_irq_20s_20261110.json`、`c5_poll_20s_20261110.json`；汇总 `c5_irq_poll_comparison_20261110.json`。
新增快照证据完整性、20 秒超时分类、参数边界与入口转发回归，Python 共 73 项通过。
汇总时再次只读检查 P4：同一 boot_id、默认 IDLE/diag=0、电源保持正常、FAULT valid=0、
触摸读取继续推进；未追加复位或刷写。

## 10. 2026-11-10 下载模式下的实际取证

- 用户重新进入下载模式后，经 P4 原生 USB → SDIO 成功识别 C5，实测 Flash 为 4 MB。
  使用官方 RAM flasher stub 读出完整 4 MB；块内与 USB 缓存 MD5、磁盘 SHA-256 通过，
  前 128 KB 独立重复读取一致。此阶段未复位 C5，全程没有写/擦除 C5 Flash。
- 分区表、bootloader 和 ota_0 校验通过；重建的历史四区域 payload SHA 与 11 月 9 日
  成功烧录记录完全一致。当前存储的应用为 network_adapter 2.12.13 / IDF v6.1。
  otadata 两份擦除、ota_1 空白，离线预测回退 ota_0，不代表实际应用已经启动。
- 为补取芯片版本，仅更新 P4 诊断固件并在 radar=0 时显式复位 C5 一次；
  下载连接成功、FACTS 有效，实际 C5 v1.2，符合镜像的 v1.0–v1.99 限制。
  不写 eFuse、不操作 GPIO6、不用 COM3，无失败触发的自动复位/刷写。
- P4 当前 ELF SHA：`079ea4ac0f00779b31ff2105848827e9283d3c5657cad0fff613a4e2ac17bea2`。
  收尾只读检查同一 boot_id、电源保持、USB、触摸读取进度及 FAULT valid=0 通过；
  屏幕未目视复验。Python 回归 91 项通过，programmer 增量构建通过。
- 当前 P4 保留 programmer profile、C5 保留下载/stub 会话。以上排查未发现镜像内容
  损坏或芯片修订号不兼容；仍不能证明 Flash 启动链/Hosted 应用正常或认定硬件损坏。
  自定义 RAM 程序、最小 Flash 诊断、分阶段 Hosted、联网、免引脚 OTA 和 GPIO 分时未验收。

方案：`docs/C5_NO_UART_TEST_PLAN.md`。证据位于本地忽略目录
`firmware/p4_gesture/test-results/`：`c5_read_backup_20261110.json`、
`c5_flash_inspection_20261110.json`、`c5_revision_facts_20261110.json`、
`c5_download_final_health_20261110.json`。整片备份可能含凭据/工厂数据，仅本地保存，不入 Git。

## 11. 2026-11-10 最小 SDIO 应用阶段（已写入，运行待验收）

- 自定义 RAM 安全加载未实现（官方 SDIO loader 的 stub 重叠/重新进 bootloader 限制），
  明确跳过 RAM 阶段；基于完整备份转入最小 Flash 应用，不冒充 RAM 执行成功。
- 新增独立诊断子工程 `firmware/c5_hosted/diagnostic` 和一键 `c5-minimal` 工具。
  prepare 同时核验备份、构建诊断、生成与备份逐字节一致的 ota_0 完整恢复包；
  恢复包仍未实际写回验收。诊断只初始化 SDIO，C5 GPIO2 保持高阻。
- 03:07 已完成实际烧写：仅 C5 ota_0 的 0x20000/150688 B，MD5 验证通过。
  不改 C5 启动程序、分区、otadata、NVS 或工厂区，没有整片擦除、失败后重刷或 COM3。
- P4 已安装配套 programmer，原生 USB 程序复位、镜像、供电保持及 FAULT 检查通过；
  当前 P4 ELF SHA：`c805c4b8100f643ae2e73e0acd76b346dbb4145b99ffea63bce1218986e673ec`。
  Python 99 项通过，编译通过，实屏未目视复验。
- 当前 C5 仍为下载/stub 会话；已请求用户解除三脚，等待明确确认后才单次复位启动。
  实际诊断 ELF 身份、20 次心跳、16 个随机 CMD53 回显均尚未验收，不能宣称应用已运行。
  Hosted/联网/免引脚 OTA/GPIO 分时仍未验收。

证据：`firmware/p4_gesture/test-results/c5_minimal_flash_20261110.json`、
`c5_minimal_flash_health_20261110.json`。操作与恢复入口见 `tools/README.md`。

## 12. 2026-11-10 解除三脚后的最小应用实测（未通过）

- 用户确认三脚全部解除后，只执行一次 DIAGRUN。C5 复位命令完成、
  1-bit/400 kHz 卡初始化通过，但 APP_SIGNATURE 等待超时（0x107），
  有效身份/心跳样本 0、回显包 0；未进入后续回显阶段，不能称作双向数据测试通过。
- 未追加 C5 复位/重刷。为补齐原始证据，仅增量构建/更新 P4，新增被动 DIAGOBSERVE；
  该模式不复位 C5、不写 Flash、不访问 FIFO，SDIO 卡初始化一次后读取共享诊断字节。
  20 次采样均成功、每次 12 B，全为零；OBSERVED 仅表示采样完成，不是应用验收成功。
- 已核对本地官方 C5 共享寄存器映射，未发现偏移错误，但这不能独立证明实际硬件读通路。
  仍无 Flash 应用执行证据，不能区分启动模式、复位/启动链、早期 SDIO 初始化或观测问题。
  不据此认定芯片损坏，不再用单纯加长 Hosted 等待/扫频代替启动证据。
- 当前 P4 ELF SHA：`075eab726c249b96c089fb3d1f4798a2a6a4f610acf860bf0ee15c35b97d7fc2`。
  被动采样前后同一 boot_id，USB、电源保持、FAULT valid=0、触摸读取进度检查通过，
  driver_err=0；实屏未目视复验。Python 回归 104 项通过，P4 增量构建通过。
- 当前 C5 Flash 是最小诊断应用，原 Hosted 尚未恢复；原应用恢复包和整片备份仍在本地。
  C5 实际启动模式、应用运行、Hosted、Wi-Fi、OTA、GPIO 分时均未验收；三脚保持解除即可。

证据：`firmware/p4_gesture/test-results/c5_minimal_run_20261110.json`、
`c5_passive_observe_20261110.json`、`c5_minimal_conclusion_20261110.json`（均本地忽略）。

## 13. 2026-11-10 启动脚低电平的新增实证（当前结论）

- 官方 ESP32-C5 Datasheet v1.5 第 35 页表 3-3 已核对：GPIO28=1 为 SPI Flash
  启动；GPIO26/27/28=000 为支持 SDIO 的 Joint Download Boot 1。
  esptool 串口说明只覆盖另一下载组合，不能据此把 000 判为无效组合。
- 增加 P4 只读 RESETTRACE 缓存：GPIO54 配置、稳定后的高/低采样、低段间隔。
  写后留 1 ms 稳定时间；SIG_GPIO_OUT_IDX=256 时不能仅凭 peripheral OE 位
  否定正常 GPIO 输出。前两版探针报告保留：第一版在脉冲前误拒默认 OE 标记，
  未拉低；第二版有脉冲但立即读数未稳定而停止，均不构成 C5 硬件复位失败的证据。
- 修正后单次 DIAGRUN：P4 pad 初始=1、低段两点=0、释放两点=1，低段间隔
  10911 us；仅证明 P4 pad，不是 C5 EN 侧波形。卡初始化通过，APP_SIGNATURE
  仍超时，有效应用样本/回显=0。本轮未写 C5 Flash、未操作 GPIO6。
- 随后仅程序复位 P4 以释放 owner，不发 C5 复位命令，单次 CONNECT 成功并执行
  官方 RAM flasher stub，得到新的 C5 FACTS：strap=0x22（锁存 GPIO27=0、GPIO28=0）；
  GPIO_IN=0x0015fd94、GPIO_ENABLE=0；MUX26=0x1a00、MUX27/28=0x1b00。
  三脚输入均开启且当前全为低；GPIO27/28 内部上拉已开、下拉未开。不是输入
  buffer 禁用的零或软件 GPIO 输出拉低。当前可确认仍为下载/stub 状态，不是 Flash 应用。
- 用户已明确解除三脚接地；不否认用户操作，但电气读数仍低。现在优先查模组
  6 脚 GPIO28 及 7/8 脚的残留接地、焊桥、接线或板载下载电路，不继续盲刷。
  不直接强接 3.3V、不驱动 GPIO6；具体下拉来源和 C5 EN 侧波形仍未知，不宣告芯片损坏。
- 当前 P4 ELF SHA：`fa8de08d5c8cabb83391ba2f6436276d90f52d09c0a6a977db3c7bc49ebeadcd`。
  C5 Flash 仍为最小诊断应用，RAM 为官方下载 stub，原 Hosted 未恢复。
  同一 stub 会话跨 P4 重启 CONNECT 曾失败一次，原报告保留，没有自动重试掩盖。
  Python 110 项及增量构建通过；最后同一 boot_id 的 USB/供电保持/FAULT=0/触摸进度
  检查通过，实屏未目视复验。Hosted、联网、OTA、GPIO 分时仍未验收。

本地报告：`firmware/p4_gesture/test-results/c5_settled_run_20261110.json`、
`c5_settled_boot_facts_20261110.json`、`c5_boot_pins_final_health_20261110.json`；
汇总 `c5_boot_pin_conclusion_20261110.json`。下一步核对三脚及接地的局部照片/连接；
当前不需要再次接地、无依据复位或重刷。

### 用户补充：三根下载脚互相并联

用户说明模组6/7/8脚一直互相连接，便于整组接地下载；此前“解除接地”不代表解除三脚互联。
因此三路当前低电平是同一公共节点的读数，不能当作三个独立下拉故障。
仅互联且不接地并不必然导致下载；仍需定位公共节点被拉低的来源。
下一步先确认互联是可拆临时线还是焊接/PCB连接；优先隔离模组6脚GPIO28做对照，
其正常启动电平必须为高，GPIO26/27的启动电平任意。保持不强接3.3V、不操作GPIO6，
未收到接线变更确认前，不追加复位/刷写，也不宣称并联本身已证实是根因。

### 补充约束与代码核查

用户目前无法分离三脚，不能再把拆分互联作为必需操作。静态核查结果：当前最小 C5
诊断应用没有将 GPIO26/27/28 同时输出高，也没有统一设置三脚弱上拉；只显式配置
GPIO2 为输入无上下拉，以及固定 SDIO 脚7/8/9/10/13/14。P4 下载器只控制其自身54复位
和14..19 SDIO，C5 FACTS 路径只读，不写启动脚。P4同号GPIO26/27/28不是C5这三脚。
上次快照：C5 26输入无上下拉，27/28输入弱上拉，GPIO_ENABLE=0，三路均低；
26无上拉不等于主动拉低，不可将读数直接归因为残留接地。未发现当前项目路径中有
三脚一高一低互相对打的配置，但未据此证明外部下拉来源。
本轮仅查代码并保存 `c5_parallel_pin_code_audit_20261110.json`，没有硬件命令、复位或烧写。
若继续软件对照，应在已运行RAM stub中先查外设输出路由，再显式将三脚统一设为
输入+弱上拉并读回；不推挽强拉高。该对照尚未实现/执行，且不能保证EN复位后保持，
把上拉代码放在未运行的Flash应用中不能解决应用之前的启动采样。

## 14. 2026-11-10 04:26 用户接入 3.3V 后的单次启动验证（当前状态）

- 用户确认三脚公共节点接到 3.3V，授权一次 C5 复位、心跳和 SDIO 回显检查。
  实时 UI 确认 radar=0 后，仅发出一次 DIAGRUN；不编译/烧录，不操作 GPIO6。
- P4 GPIO54 pad 读回 1→0→0→1→1，低段 10890 us；仅为 P4 pad 证据，
  未独立测得 C5 CHIP_PU 波形或三脚电压。SDIO 1-bit/400 kHz 卡初始化通过。
- APP_SIGNATURE 超时（263 / 0x107），12 次共享诊断字节读取成功且全零，
  有效身份/心跳样本 0；未进入回显阶段，不将 echoes=0 当作回显链路已测试。
- 当前 FACTS USB 接口只返回 CONNECT 时缓存，不能验证用户刚拉高后的电平；
  旧 GPIO_IN=0 和旧 strap 不能当作本次新结果。当前三脚电平、启动模式、RAM 内容
  均未确认；C5 Flash 仍为最小诊断应用，原 Hosted 未恢复。
- 失败即停，未追加 C5/P4 复位、CONNECT 或重刷。P4 boot_id 保持 bf958202，
  USB/电源保持/FAULT valid=0/触摸读取进度通过，driver_err=0；实屏未复验。
  不据此认定拉高失败或芯片损坏。下一步优先建立实时采样/独立启动证据，
  不重复同样的复位测试，Hosted/Wi-Fi/OTA/GPIO 分时仍未验收。

本地报告：`firmware/p4_gesture/test-results/c5_manual_3v3_run_20261110.json`。

## 15. 2026-11-10 04:34 持续测试收尾：需要核对真实接线（当前结论）

- 后续未重刷/再复位 C5。仅程序重启 P4 释放 SDIO owner；新 boot_id=eee879ea，
  从新 IDLE 单次 CONNECT 成功并执行官方 RAM stub。P4重启期间的C5 EN侧波形未测。
- 04:31:58–04:31:59 的新连接采样仍读 GPIO26/27/28=000，输入均开启，
  GPIO27/28弱上拉已开，GPIO_ENABLE=0，strap=0x22。已重新核对本地官方 C5
  寄存器地址与位定义；不是复用03:43缓存，不是万用表测得0V，也未排除全部外设输出路由。
- 同一stub会话只读回281760 B启动区与诊断应用，逐块目标/USB MD5通过；
  与原备份+诊断应用逐字节一致，bootloader/应用 checksum/SHA-256通过。
  下载链路、官方RAM代码执行与Flash读取可用；正常Flash启动/应用心跳仍未验证，
  不足以认定芯片损坏。C5仍为最小诊断Flash+官方RAM stub，原Hosted未恢复。
- 到达人工边界：用户所述接3.3V与芯片新采样为低不一致，需要核对模组6/7/8公共节点、
  3.3V实际取电点/公共GND与连接。优先请求清晰接线近照；不要求拆分互联、COM3或
  用户没有的测量设备。不继续盲刷/重复复位，不把未知下拉来源写成定论。
- 修正PC端`c5-program --facts`：已有CONNECTED/DONE时拒绝把缓存冒充新测量，
  只有本轮IDLE→新CONNECT才接收为本轮采样；底层read_facts明确标注CONNECT_CACHE，
  nonce只验证USB回复，不证明硬件新采样。112项Python测试通过，未重编/烧录固件。
- P4 USB/供电保持/FAULT valid=0/触摸读取进度通过，GPIO6未操作，实屏未复验。

本地证据：`firmware/p4_gesture/test-results/c5_3v3_fresh_connection_20261110.json`、
`c5_3v3_startup_readback_20261110.json`、`c5_continuous_conclusion_20261110.json`。

## 16. 2026-11-10 05:05 全局只读复查（当前状态）

- 设备已在两次检查之间重启：当前P4 boot_id=b68a43de，不再是先前eee879ea。
  本轮未发P4/C5复位、未烧录/上传RAM助手，未动GPIO6，未用COM3。
- P4固件身份匹配，USB/供电保持/UI帧计数/180°状态/触摸读取正常，FAULT valid=0；
  首页eyes，亮度100%，radar=0，wifi=UNAVAILABLE；实屏和真实触摸手势未复验。
- **P4 reset_reason=7（官方IDF ESP_RST_WDT），marker_retained=false。**
  上次看门狗类复位触发原因未知，不将当前运行检查通过或FAULT无有效记录当成复位正常。
  已追加约30秒只读稳定性采样，结果见最终JSON，不作为长期稳定性结论。
- C5初始加载器IDLE，FACTS valid=0，旧下载/stub会话状态不再沿用，当前三脚电平未知。
  单次DIAGOBSERVE卡初始化及20次CMD52读取通过，但诊断字节全零，有效心跳/身份0；
  未测CMD53回显，Flash应用/Hosted/Wi-Fi仍未验收，本轮未新读Flash或建立下载连接。
- 112项Python回归通过。全局仅部分通过：P4当前运行可用，上次WDT需排查，
  C5仅基础SDIO应答成立，正常应用执行与三脚接线问题尚未解决。

本地报告：`firmware/p4_gesture/test-results/global_status_final_20261110_0505.json`、
`global_c5_observe_20261110_0505.json`。

## 17. 2026-11-10 16:14 三脚互联解除后的实测（当前状态）

- 用户现已解除模组6/7/8脚之间的互联；不再沿用“无法分离”的旧约束，也不把旧000
  快照当成三脚分离后的电平。此前USB消失，本轮已恢复原生USB COM10和匹配P4身份。
- P4电源保持/触摸读取/FAULT valid=0通过。复位前实时UI确认radar=0，执行一次DIAGRUN：
  P4 GPIO54 pad读回1→0→0→1→1，低段10922us；仅为P4侧证据，不证明C5 EN侧波形。
  SDIO卡初始化成功，12次诊断读取完整且全零，APP_SIGNATURE超时263/0x107，
  有效应用身份/心跳0，未进入回显阶段。没有追加C5复位或写Flash。
- 为获取新寄存器样本，仅程序重启P4一次释放owner，然后单次无C5复位命令CONNECT。
  本次在CONNECT阶段返回ESP_LOADER_ERROR_INVALID_TARGET=6，未取得新FACTS。
  这不是三脚仍低的证据，也不能直接认定芯片型号错误、已正常启动或芯片损坏。
  当前C5执行模式/三脚电平均未知，不能沿用之前官方下载stub运行的结论。
- 当前P4 boot_id=b1b91e91，软件重启原因3，marker保留、USB/电源/FAULT/触摸读取通过，
  无需SW1；此前上电boot的WDT=7记录仍保留，不能因本轮软件复位而判定已修复。
  本轮未编译/烧录、未操作GPIO6、未用COM3；实屏未重新验收。
- 下一步需采集SDIO CIS原始身份/诊断初始化证据，区分下载身份与应用SDIO身份及读取路径；
  不盲重刷、不重复C5复位，也不继续让用户拆已分离的三脚。诊断Flash仍为上次写入的
  最小应用，原Hosted未恢复（本轮未重新读Flash核对），Wi-Fi/OTA仍未验收。

本地证据：`firmware/p4_gesture/test-results/c5_separated_run_20261110_1612.json`、
`c5_separated_fresh_facts_20261110_1613.json`、`c5_separated_conclusion_20261110.json`。

## 18. 2026-11-10 新开发板：C5 SDIO 免按键升级准备

- 用户新接入可切换 P4/C5 UART 的开发板；它和原自制板分开管理，后续烧哪颗芯片必须明确告知。
- 当前 COM4/CH343 接收的双核 WDT 日志疑似 P4，路由/板型尚未确认，没有 C5 启动身份；未烧录新板。
- 已修复 C5 OTA 会话/首包/边界/激活错误处理，加入启动身份与阶段日志；构建、镜像离线验证通过。
- 113 项 unittest 通过，包含真实 C handler 的 35 场景 × 2 配置故障注入；不是实机 SDIO 升级验收。
- 需先切至 C5 UART，确认板型/修订号/Flash/启动日志，再决定首次 C5 下载和新板 P4 测试 profile。
- 宿主 SDIO OTA 上传工具、重连身份验证、连续 A/B 升级、掉电/回滚/ROM 恢复仍未完成；不宣称稳定免操作已实现。
- 当前候选、烧录边界与后续验收见 `docs/C5_SDIO_OTA_BRINGUP.md`；保留 GPIO6，不套用旧板共享 GPIO54 的控制逻辑。

## 19. 新开发板 P4 已烧录，等待 C5 UART

- 用户确认同型核心板、手动切换 UART；要求 C5 应用失效时也能自动恢复，最终迁焊回自制板。
- COM4 ROM识别 P4v3.2/16MB。原厂 CO5300演示并非空白；已替换为独立无屏 SDIO诊断，
  ROM `--no-stub` 三段写入hash通过，NVS/storage保留。C5 Flash未写。
- 诊断P4 ELF=`526f21d8381fa09767b409baaa59ef2acb880b2a9412b2239e3aff9292763046`，
  INFO和连续心跳验证；首捕获GBK异常已修复，未据工具异常重刷。启动原因7仍记录，来源未排除。
- 基线及一次 GPIO54复位请求后，两轮SDIO卡初始化/CIS/5次12字节读取/owner释放均通过，
  CIS0092:6666、共享数据全零。未运行Hosted RPC，不能当作C5固件运行或损坏结论。
- 118项回归通过。已请求切C5 UART；读日志/芯片版本后才决定首次C5下载。
- 自动恢复不能止于运行时OTA，需后续受保护恢复固件/bootloader/失败回退设计和实机验收；尚未实现。
- `tools/p4_bringup.py`使用本机config统一路径；该无屏程序不含GPIO29保持/产品USB，
  迁回自制板前必须回归集成产品P4固件，不能直接用此诊断程序交付。

## 20. 新开发板 C5 出厂 Hosted 启动已确认

- 2026-11-10 17:49–17:52 的180秒UART接收已完成：C5v1.2/4MB，正常Flash启动，
  network_adapter / ESP-Hosted2.12.9 / IDF6.0.2，SDIO stream初始化成功，统计持续至150529ms。
  当前未有主机Hosted握手，流量0不是C5故障结论；出厂C5不需要盲刷。
- 出厂槽ota_0=0x20000、ota_1=0x200000，各0x1e0000；仅取得ELF前缀830470ccd，
  没有完整镜像/备份/完整SHA。窗口内前两个POWERON启动中断，不能排除用户操作而认定重启循环。
- 新C5未写Flash/运行stub/同步ROM；本轮仅接收与一次help，不能用无CLI响应判RX故障。
- 已请求UART切回P4。新增独立无屏Hosted握手profile，保留raw基线；单owner，一次显式CONNECT，
  再以10轮实时版本/芯片RPC验证，不启用Wi-Fi、不用GPIO6、不提供Flash写命令。
  构建与125项软件回归已通过；app605792B，ELF=e1bc3d02d6293f6019b38616bc64a8197af4ad3199871086d89d2c83971799f4，
  镜像checksum/digest通过，分区表与raw一致。尚未烧录该profile，未宣称Hosted/OTA/失效恢复验收通过。
- 本节新开发板结论不覆盖第17节旧自制板的Flash内容和失败证据。详见docs/C5_SDIO_OTA_BRINGUP.md。

## 21. 新开发板 P4/C5 Hosted 实机通信通过（2026-11-10 18:37）

- 用户确认UART切回P4后，COM4经ROM再次识别P4v3.2/16MB，只写P4三段并hash通过；
  运行ELF=e1bc3d02d6293f6019b38616bc64a8197af4ad3199871086d89d2c83971799f4。
- Hosted单次CONNECT请求一次C5复位，1-bit400kHz收到真实INIT并完成版本/芯片RPC；
  C5为出厂esp32c5/chip_id23/fw2.12.9，不需要先用UART刷C5。
- CONNECT一组+首轮10组+追加100组，共111组/222次实时RPC全部通过；
  11轮VERIFY逐项核对sample0..9且终态成功。末端P4 uptime203298ms，采集间无重启或新panic/task_wdt。
- 启动原因reset7仍未定位，不因通信通过宣称看门狗已修复；只验收低速RPC，
  Wi-Fi业务、40MHz、运行时OTA、C5应用失效恢复、迁回自制板均未验收。
- C5 Flash未写，GPIO6/三脚未操作；UART保持P4/COM4，所有采集已关闭。
- 下一步实现app-only OTA+新镜像重连身份验证，写C5前明确通知；自动失败恢复独立实现，
  不能把当前通信通过或双OTA槽当成完成免引脚升级/恢复。
- 本地汇总：firmware/p4_gesture/test-results/new_devboard_hosted_acceptance_20261110.json。

## 22. C5维护基线已安装，P4启动隔离与SDIO升级验收（2026-11-10）

- C5首次UART安装已完成：460800下整片4MB备份、ota_1/回滚bootloader/otadata有限写入，
  每段hash及独立读回通过；整片对比证明重启前其他扇区（含原厂ota_0/NVS/表/标定）保留。
  备份含敏感数据仅留本地，不进Git；后续SDIO A/B升级将正常改写非运行应用槽，不再承诺保留原厂ota_0。
- 用户已切回P4。实时SDIO维护身份esp32c5/chip_id23/fw2.12.13、完整ELF b97127f1…、ota_1 VALID通过；
  共享GPIO2弱上/下拉两端数字读回、500ms租约到期高阻通过，未做强推挽或产品音频验收。
- 原P4维护双核固件复位时偶发在app_main前ROM memset产生Store access fault；
  延后Hosted初始化后20轮计划在第19轮仍失败（18轮通过），另一独立30轮通过不抵消失败。
  底层根因未确定，失败报告保留，不重启掩盖、不关闭PMP/栈保护/看门狗。
- 维护专用单核400MHz/UART0 TX37 RX38/460800已烧P4，未再见该启动panic；但两批100轮计划分别在
  第96/47轮ATTACH/RPC失败，不能称100轮通过。暖重连计数不同步为证据支持的推断，尚未根治。
- 上传前`--recover-baseline`一次EN复位建立旧VALID基线；ACTIVATE后仅P4复位/ATTACH保留新候选。
  10轮完整1316640B SDIO升级、PENDING→确认VALID通过；中断/超时/启动失败/SDIO失联恢复全部实测通过。
- 工具增加reset-soak、短ACK读取、实际UART baud、panic尾部保留及挂起注入保护；173项离线回归通过。
- 本轮专用配置不代表产品双核已修复，也不含自制板电源保持/nativeUSB/UI/音频仲裁集成。
  SDIO连续升级、中断、未确认超时和故障镜像回滚的实际结果见docs/C5_MAINTENANCE.md，不以离线测试替代。

## 23. 开发板免引脚维护验收完成（2026-11-11，限定故障模型）

- 正常应用ELF b97127f1…；10轮A/B更新全部完整ACK/新槽PENDING/确认VALID，无失败补救。
- 中断只写4096B未激活，旧VALID保留；未确认候选90秒自回退；启动前自行失败的候选由bootloader回退。
- 新增独立build_c5_hang：候选不启动Hosted、不运行90秒维护任务；超过95秒仍SDIO失联，
  P4响应正常，电脑工具单次EN使bootloader回退旧VALID。该测试不模拟EN/电源/Flash硬件损坏。
- 最终实时检查回到ota_1 0x200000、正常完整ELF、VALID、GPIO2 HIZ/lease0；串口已关闭。
- 实测报告c5_sdio_update_first/nine、interrupted、unconfirmed、fault、hung、pins、check_20261111.json，
  详见docs/C5_MAINTENANCE.md。173项离线回归通过，和实机证据分开记录。
- 下一阶段：把维护服务接入产品P4现有原生USB；保留GPIO29供电保持、显示暖启动、触摸/180°旋转，
  完成音频静音/I2S停止/共享GPIO6释放与恢复，再在自制板回归后决定迁回焊接。
- 不把开发板专用单核隔离镜像直接烧回自制板；双核Store fault根因及任意P4暖重连仍未根治。

## 24. Wi-Fi专项验证（2026-11-11，手机热点基础联网通过）

- 只更新P4维护profile（当前ELF 5261a506…），旧e8b5f673…镜像留存；C5维持b97127f1…正常固件，未重刷。
- 密码不入代码/Git/报告，仅UART临时下发，NVS禁用、RAM存储，结束清空配置并停Wi-Fi。
- 新增NET_SCAN/JOIN/PROBE/STOP与tools/c5_wifi.py；HTTPS要求CA/域名/有效期，PC UTC提供诊断时钟，不等于SNTP验证。
- 初次在启动前设置双频返回0x3002，已修正P4调用顺序；旧失败报告保留。
- 修订后成功扫描目标：channel8/auth7/RSSI -92dBm，关联reason4断开，未得到IP，故DNS/HTTPS尚未执行。
  失败报告c5_wifi_acceptance_20261111_r2.json保留；清理、原boot/ELF/VALID/HIZ保持通过。
- 187项离线回归通过；后续依用户要求查找热点，电脑扫描得到完整SSID后，由C5真实扫描/连接验证。
- 手机热点实测报告`c5_wifi_hotspot_20261111.json`为true：扫描RSSI -71dBm/channel8/auth3，
  DHCP IP 172.20.10.3，连接RSSI -67dBm；example目标连续3轮DNS及HTTPS 200、577B完整响应、证书校验通过。
  espressif目标3轮DNS/TLS成功但HTTP 302/0B，未计为严格通过，`all_targets_ok=false`，不隐藏该差异。
- 本轮未编译/刷P4或C5、未操作下载脚；测试前显式基线EN复位一次，联网期间C5 boot 5a2fda8c/正常完整ELF/ota_1 VALID/HIZ保持。
  测试结束清空RAM凭据并断开/停止/反初始化Wi-Fi，串口已释放；不是持久在线配置，也不是多小时/反复关联验收。
- 手机热点通过说明当前P4→SDIO→C5链路具备真实联网能力；家用AP失败根因、天线状态、BLE和产品UI集成仍未验收。
