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
```

硬件流程只使用主板原生 USB，禁止 COM3/WCH-Link；首次冷启动仍需要板上开机动作。
Hosted profile 不代表 C5 已烧录或 Wi-Fi 已联网，GPIO6 保留给音频，GPIO54 与雷达 SOP0
的共享复位风险仍需实机验证。
