#!/usr/bin/env python3
"""DEMO #1 一键测试：自动下载 + OLED/LVGL + 触摸。

板子只接一根 USB-C，不需要按 BOOT / EN / 电源键（首次空片烧录除外）。
脚本把「触发下载 -> 等 ROM 口 -> 烧三镜像 -> hard-reset 回应用 -> 读 CDC 日志
-> 发测试指令 -> 断言」串成一条命令，并给出 PASS / FAIL 报告。

    python demo_test.py --check         # 不接板也能跑：校验编译产物与命令
    python demo_test.py --run           # 完整跑一遍（推荐）
    python demo_test.py --monitor       # 只打开应用口看实时日志，手动摸屏
    python demo_test.py --flash-only    # 只烧写，不跑日志断言

USB 状态机：
    应用态 303A:4005 (TinyUSB CDC+HID)
      -- CDC 写 "MEOAVE-DOWNLOAD\\n" -->
    固件 tud_disconnect() + LP_SYS pad hold 锁存 PWR_HOLD
      + LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT(bit2) + esp_restart()
    ROM 下载态 303A:0012 (esptool ESP32P4ROM.IMAGE_CHIP_ID=18)
      -- esptool --before no-reset ... --after hard-reset -->
    watchdog_reset -> 应用态 303A:4005
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

try:
    import flash_usb as fu          # 复用端口发现 / magic / esptool 命令拼装
    import serial
except ImportError as exc:
    sys.exit(f"缺少依赖 ({exc})：python -m pip install pyserial")


# ---------------------------------------------------------------- 工具

def hr(title=""):
    print("\n" + "=" * 68)
    if title:
        print(title)
        print("=" * 68)


def preflight(build_dir, no_stub, baud):
    hr("[0/5] 预检（不需要接板）")
    ok = True
    args_file = build_dir / "flasher_args.json"
    if not args_file.is_file():
        print(f"  FAIL 缺少 {args_file}，请先 full build")
        return False
    cmd = fu.build_flash_command(build_dir, "COMx", no_stub, baud)
    for off, rel in sorted(__import__("json").loads(
            args_file.read_text(encoding="utf-8"))["flash_files"].items(),
            key=lambda kv: int(kv[0], 0)):
        p = Path(rel)
        if not p.is_absolute():
            p = build_dir / p
        exists = p.is_file()
        print(f"  {'OK  ' if exists else 'FAIL'} {off:>8}  {p.name:<28} "
              f"{p.stat().st_size if exists else 0:>9} B")
        ok &= exists
    try:
        r = subprocess.run([sys.executable, "-c", "import esptool"],
                           capture_output=True, timeout=10)
        print(f"  {'OK  ' if r.returncode == 0 else 'FAIL'} esptool 可用")
        ok &= (r.returncode == 0)
    except Exception as exc:
        print(f"  FAIL esptool 检查失败: {exc}")
        ok = False
    print(f"  INFO 应用口 PID 303A:4005 / ROM 下载口 PID 303A:0012")
    print(f"  INFO 烧写命令:")
    print("       " + " ".join(str(c) for c in cmd))
    ports = fu.espressif_ports()
    print(f"  INFO 当前 Espressif USB 设备: {len(ports)}")
    for p in ports:
        print("       " + fu.describe(p))
    return ok


def trigger_and_flash(args, build_dir):
    hr("[1/5] 请求无感下载")
    already = fu.rom_port()
    if already:
        print(f"  已在 ROM 下载态: {fu.describe(already)}")
        port = already.device
    else:
        target = args.port or (fu.app_port().device if fu.app_port() else None)
        if not target:
            print("  FAIL 没找到 303A 设备。检查 USB 线、3V3、VDD_USBPHY、R13/HOLD。")
            return None
        print(f"  应用口 {target}，写 CDC magic {fu.MAGIC!r}（不需要按任何键）")
        try:
            fu.send_magic(target)
        except Exception as exc:
            print(f"  WARN magic 发送失败: {exc}")
        rom, saw_disc = fu.wait_for_rom(timeout=args.timeout)
        if not rom:
            print("  未见到 ROM 口，追加 DTR/RTS 复位脉冲 ...")
            fu.send_reset_pulse(target)
            rom, saw_disc = fu.wait_for_rom(timeout=args.timeout)
        if not rom:
            print("  FAIL ROM USB 下载口 303A:0012 从未出现。")
            print("       排查：PWR_HOLD pad hold 是否被清、R13 下拉是否导致掉电、")
            print("             DIS_USB_OTG_DOWNLOAD_MODE eFuse、首次烧录需按住 BOOT。")
            return None
        port = rom.device
        print(f"  OK   ROM 口 {fu.describe(rom)}（观察到断开={saw_disc}）")
        time.sleep(0.4)

    hr("[2/5] 烧写三个镜像")
    cmd = fu.build_flash_command(build_dir, port, args.no_stub, args.baud)
    print("  " + " ".join(str(c) for c in cmd))
    rc = fu.run(cmd, False)
    if rc != 0 and not args.no_stub:
        print("  stub 失败，改用 --no-stub 重试 ...")
        rc = fu.run(fu.build_flash_command(build_dir, port, True, args.baud), False)
    if rc != 0:
        print(f"  FAIL esptool 返回 {rc}")
        return None
    print("  OK   烧写完成，esptool hard-reset（USB-OTG 下走 watchdog_reset）")
    return port


def wait_app(args):
    hr("[3/5] 等待应用态 CDC+HID 重新枚举")
    p, rom_gone = fu.wait_for_app(timeout=args.app_timeout)
    if not p:
        print("  FAIL 应用口 303A:4005 未出现（FORCE_DOWNLOAD_BOOT 可能没被清 / 掉电）")
        return None
    print(f"  OK   {fu.describe(p)}（ROM 口已消失={rom_gone}）")
    return p.device


# ---------------------------------------------------------------- 日志验证

CHECKS = [
    ("USB 枚举",        lambda L: any(x.startswith("DEMO:USB ") and "pid=4005" in x for x in L)),
    ("LVGL 启动",       lambda L: any("DEMO:RESULT" in x and "LVGL=OK" in x for x in L)),
    ("DPI 真的在刷",    lambda L: any(_vsync_of(x) > 0 for x in L if x.startswith(("DEMO:BEAT", "DEMO:DISPLAY")))),
    ("CDC 命令 PING",   lambda L: any(x.startswith("DEMO:PONG") for x in L)),
    ("触摸注入/绘制",   lambda L: any(x.startswith("DEMO:TOUCH ") and "src=CDC" in x for x in L)),
    ("I2C 触摸扫描",    lambda L: any(x.startswith("DEMO:SCAN ") for x in L)),
    ("USB HID 上报",    lambda L: any(x.startswith("DEMO:HID ") for x in L)),
    ("LVGL 换底色",     lambda L: any(x.startswith("DEMO:COLORS ") for x in L)),
]


def _vsync_of(line):
    for tok in line.split():
        if tok.startswith("vsync="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return 0
    return 0


def read_logs(port, seconds, baud, commands=True):
    hr(f"[4/5] 读取应用日志 / 发送 DEMO 指令（{seconds}s）")
    lines = []
    t_start = time.time()
    t_end = t_start + seconds
    sent = False
    try:
        with serial.Serial(port, baud, timeout=0.15, write_timeout=2) as s:
            s.dtr, s.rts = True, True      # 让固件的 CDC 日志转发打开
            s.reset_input_buffer()
            s.write(b"PING\n")             # 早期就能回，确认 CDC 双向通
            s.flush()
            while time.time() < t_end:
                # 纯色自检约 7s，等 DEMO:UI_READY 再发依赖 LVGL 的指令；
                # 超时兜底也发一次，保证即使 UI 起不来也能拿到诊断。
                ready = any(x.startswith("DEMO:UI_READY") for x in lines)
                if not sent and commands and (ready or time.time() > t_start + 14):
                    for c in ("STATUS", "SCAN", "TOUCH 230 230", "HID", "COLORS", "PING"):
                        s.write((c + "\n").encode())
                        s.flush()
                        time.sleep(0.15)
                    sent = True
                try:
                    raw = s.readline()
                except Exception:
                    break
                if not raw:
                    continue
                text = raw.decode("utf-8", "replace").rstrip("\r\n")
                if text:
                    lines.append(text)
                    print("  | " + text)
    except Exception as exc:
        print(f"  WARN 读取日志中断: {exc}")
    return lines


def monitor(port, baud):
    hr("实时监控（Ctrl+C 退出）。发送 HELP / TOUCH x y / DOWNLOAD 可交互")
    try:
        with serial.Serial(port, baud, timeout=0.2) as s:
            s.dtr, s.rts = True, True
            while True:
                raw = s.readline()
                if raw:
                    print(raw.decode("utf-8", "replace").rstrip())
    except KeyboardInterrupt:
        print("\n结束")
    except Exception as exc:
        print(f"串口错误: {exc}")


def report(lines):
    hr("[5/5] 判定")
    passed = 0
    for name, fn in CHECKS:
        ok = bool(fn(lines))
        passed += ok
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    survived = any("DEMO:PWR " in x and "survived=1" in x for x in lines)
    print(f"  {'PASS' if survived else 'WARN'}  复位期间 3V3 未掉电 (PWR_HOLD pad hold)"
          if survived else
          "  WARN  未见 survived=1：本次可能是冷启动，或 R13 下拉导致复位掉电（务必上板确认）")
    touch = any("DEMO:RESULT" in x and "TOUCH=OK" in x for x in lines)
    print(f"  {'PASS' if touch else 'WARN'}  CST820 物理触摸（NO_CTP 时用 CDC 注入验证渲染链路）")
    print(f"\n  必须项 {passed}/{len(CHECKS)} 通过  ->  "
          f"{'DEMO #1 PASS' if passed == len(CHECKS) else 'DEMO #1 FAIL'}")
    return passed == len(CHECKS)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="MEOAVE DEMO #1 一键测试")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="只做无板预检")
    g.add_argument("--run", action="store_true", help="完整流程（默认）")
    g.add_argument("--monitor", action="store_true", help="只开应用口看实时日志")
    g.add_argument("--flash-only", action="store_true", help="只烧写")
    ap.add_argument("--port", help="强制指定应用态 CDC 端口，如 COM4")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--baud", default="115200")
    ap.add_argument("--no-stub", action="store_true")
    ap.add_argument("--timeout", type=float, default=12.0, help="等 ROM 口的秒数")
    ap.add_argument("--app-timeout", type=float, default=25.0, help="等应用口重枚举的秒数")
    ap.add_argument("--log-seconds", type=float, default=20.0,
                    help="读日志时长；默认 20s 覆盖 5 色 x 1s 自检 + LVGL 启动")
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = HERE / build_dir

    if not preflight(build_dir, args.no_stub, args.baud):
        return 2
    if args.check:
        hr("预检完成（未接板，未烧写）")
        return 0

    if args.monitor:
        p = args.port or (fu.app_port().device if fu.app_port() else None)
        if not p:
            print("没找到应用口 303A:4005")
            return 2
        monitor(p, args.baud)
        return 0

    port = trigger_and_flash(args, build_dir)
    if not port:
        return 3
    if args.flash_only:
        wait_app(args)
        return 0
    app = wait_app(args)
    if not app:
        return 4
    time.sleep(0.8)                    # 让 Windows 完成 CDC 驱动绑定
    lines = read_logs(app, args.log_seconds, args.baud)
    return 0 if report(lines) else 5


if __name__ == "__main__":
    sys.exit(main())
