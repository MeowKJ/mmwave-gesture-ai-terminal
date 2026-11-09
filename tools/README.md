# Meoave 工具入口

所有机器相关路径放在未纳入 Git 的 `tools/config.toml`，不要把本机用户名、ESP-IDF
安装路径或虚拟环境路径写进源码。第一次使用：

```powershell
Copy-Item tools/config.example.toml tools/config.toml
python tools/bootstrap.py
python tools/meoave.py doctor
python tools/meoave.py all
```

常用命令：

```powershell
python tools/meoave.py build                  # 产品固件（秒级增量编译）
python tools/meoave.py build --reconfigure    # 强制重新执行 CMake 配置
python tools/meoave.py build --profile hosted # P4C5 SDIO Hosted 静态构建
python tools/meoave.py test                  # 主机回归测试
python tools/meoave.py all --hosted           # 产品 + Hosted + 测试
python tools/meoave.py hardware --build       # 原生 USB 下载/复位验收
python tools/meoave.py clean                  # 仅删除两个生成目录
python tools/meoave.py fonts --fetch          # 下载字体源包并校验 SHA-256
python tools/meoave.py fonts --set round      # 切换字体集并重生成 LVGL 字库
python tools/meoave.py fonts --list           # 查看可选字体集
```

字体源文件不进 Git，默认从本仓库的 `fonts-v1` Release 下载
（`meoave-fonts-v1.zip`，75 MB，含六套 OFL 字体与逐文件校验和）；
`--fetch` 会先走直链，失败再走 GitHub API 资源地址，断点续传并校验 SHA-256。
改文案要增删汉字时，先补 `firmware/p4_gesture/main/fonts/manifest.json` 的符号集再重生成。

硬件流程只使用主板原生 USB，禁止 COM3/WCH-Link；首次冷启动仍需要板上开机动作。
Hosted profile 不代表 C5 已烧录或 Wi-Fi 已联网，GPIO6 保留给音频，GPIO54 与雷达 SOP0
的共享复位风险仍需实机验证。
