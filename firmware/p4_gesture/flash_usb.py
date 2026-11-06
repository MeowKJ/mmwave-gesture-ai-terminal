#!/usr/bin/env python3
"""Flash the board over its single USB-C port, with no buttons to press.

Hardware: ESP32P4C5 module pins 48/49 are the P4 USB 2.0 High-Speed OTG
dedicated USB_DM/USB_DP pads. The same pair is used in two states:

  * application : TinyUSB composite CDC-ACM + HID, VID 0x303A / PID 0x4005
  * ROM download: Espressif USB-OTG CDC,       VID 0x303A / PID 0x0012
                  (0x0012 = esptool ESP32P4ROM.IMAGE_CHIP_ID = 18)

This script:
  1. finds the application CDC port,
  2. writes ``MEOAVE-DOWNLOAD\\n`` to it; the firmware disconnects USB, sets
     LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT and calls esp_restart(). A DTR/RTS
     pulse is sent as a fallback for older firmware that only watches line state,
  3. waits for the ROM download device (PID 0x0012) to enumerate,
  4. flashes every image from build/flasher_args.json with --before no-reset.

Usage:  python flash_usb.py [--port COMx] [--no-stub] [--dry-run]
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required: python -m pip install pyserial")

ESP_VID = 0x303A
ROM_PIDS = {0x0012}          # esptool ESP32P4ROM.IMAGE_CHIP_ID = 18 -> USB-OTG PID
APP_PIDS = {0x4005}          # TinyUSB default 0x4000 | CDC(1) | HID(4)
MAGIC = b"MEOAVE-DOWNLOAD\n"
RESET_PULSE = ((False, True), (True, False), (True, True))


def espressif_ports():
    return [p for p in list_ports.comports() if (p.vid or 0) == ESP_VID]


def rom_port():
    return next((p for p in espressif_ports() if (p.pid or 0) in ROM_PIDS), None)


def app_port():
    ports = espressif_ports()
    # 优先精确匹配应用态 PID，避免把其它 Espressif 设备误当成下载目标
    p = next((p for p in ports if (p.pid or 0) in APP_PIDS), None)
    if p:
        return p
    return next((p for p in ports if (p.pid or 0) not in ROM_PIDS), None)


def describe(p):
    return f"{p.device} VID={p.vid:04X} PID={p.pid:04X} {p.product or ''}"


def send_magic(port_name):
    """Primary trigger: an explicit CDC command, immune to Windows usbser.sys
    coalescing SET_CONTROL_LINE_STATE requests."""
    with serial.Serial(port_name, 115200, timeout=0.1, write_timeout=2) as s:
        s.dtr, s.rts = True, True
        time.sleep(0.05)
        s.write(MAGIC)
        s.flush()
        time.sleep(0.15)


def send_reset_pulse(port_name):
    """Fallback trigger for firmware that only reacts to DTR/RTS."""
    try:
        with serial.Serial(port_name, 115200, timeout=0.1, write_timeout=1) as s:
            s.dtr, s.rts = True, True
            time.sleep(0.1)
            for dtr, rts in RESET_PULSE:
                s.dtr, s.rts = dtr, rts
                time.sleep(0.1)
    except Exception as exc:
        print(f"  (pulse skipped: {exc})")


def wait_for_rom(timeout=12.0):
    deadline = time.time() + timeout
    saw_disconnect = False
    while time.time() < deadline:
        p = rom_port()
        if p:
            return p, saw_disconnect
        if not app_port():
            saw_disconnect = True
        time.sleep(0.25)
    return None, saw_disconnect


def wait_for_app(timeout=20.0):
    """烧完 esptool --after hard-reset 后等应用态 CDC+HID (303A:4005) 重新枚举。"""
    deadline = time.time() + timeout
    rom_gone = False
    while time.time() < deadline:
        p = app_port()
        if p:
            return p, rom_gone
        if not rom_port():
            rom_gone = True
        time.sleep(0.25)
    return None, rom_gone


def build_flash_command(build_dir, port, no_stub, baud="115200"):
    args_file = Path(build_dir) / "flasher_args.json"
    if not args_file.is_file():
        sys.exit(f"{args_file} not found - build the project first")
    data = json.loads(args_file.read_text(encoding="utf-8"))
    settings = data.get("flash_settings", {})

    cmd = [sys.executable, "-m", "esptool",
           "--chip", "esp32p4", "-p", port, "-b", baud,
           "--before", "no-reset", "--after", "hard-reset"]
    if no_stub:
        cmd.append("--no-stub")           # global option: must precede write-flash
    cmd.append("write-flash")
    for key, flag in (("flash_mode", "--flash-mode"),
                      ("flash_size", "--flash-size"),
                      ("flash_freq", "--flash-freq")):
        if settings.get(key):
            cmd += [flag, str(settings[key])]

    files = sorted(data.get("flash_files", {}).items(), key=lambda kv: int(kv[0], 0))
    if not files:
        sys.exit("no flash_files in flasher_args.json")
    for offset, rel in files:
        path = Path(rel)
        if not path.is_absolute():
            path = Path(build_dir) / path
        if not path.is_file():
            sys.exit(f"missing image: {path}")
        cmd += [offset, str(path)]
    return cmd


def run(cmd, dry_run):
    print("  " + " ".join(str(c) for c in cmd))
    if dry_run:
        return 0
    return subprocess.call([str(c) for c in cmd])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="force the application CDC port")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--baud", default="115200")
    ap.add_argument("--no-stub", action="store_true", help="write via ROM commands only")
    ap.add_argument("--dry-run", action="store_true", help="print the esptool command only")
    ap.add_argument("--timeout", type=float, default=12.0)
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = Path(__file__).resolve().parent / build_dir

    print("Espressif USB devices:")
    for p in espressif_ports():
        print("  " + describe(p))

    if args.dry_run:
        run(build_flash_command(build_dir, args.port or "COMx", args.no_stub, args.baud), True)
        return 0

    already_rom = rom_port()
    if already_rom:
        print(f"[1/3] already in ROM download mode: {describe(already_rom)}")
        port = already_rom.device
    else:
        port = args.port or (app_port().device if app_port() else None)
        if not port:
            sys.exit("No Espressif USB device found. Check cable, VDD_USBPHY and PWR_HOLD (R13).")

        print(f"[1/3] requesting download on {port} (CDC magic) ...")
        try:
            send_magic(port)
        except Exception as exc:
            print(f"  magic failed ({exc})")

        found, saw_disconnect = wait_for_rom(timeout=args.timeout)
        if not found:
            print("  no ROM device yet; retrying with DTR/RTS pulse ...")
            send_reset_pulse(port)
            found, saw_disconnect = wait_for_rom(timeout=args.timeout)
        if not found:
            sys.exit("ROM USB download port (303A:0012) never appeared. "
                     "Check: R13->VBUS_ON, VDD_USBPHY, DIS_USB_OTG_DOWNLOAD_MODE eFuse, "
                     "or hold the power key during the first flash.")
        port = found.device
        print(f"[2/3] ROM download device: {describe(found)} "
              f"(saw USB disconnect={saw_disconnect})")
        time.sleep(0.4)                   # let the new COM port settle

    cmd = build_flash_command(build_dir, port, args.no_stub, args.baud)
    print("[3/3] flashing ...")
    rc = run(cmd, False)
    if rc != 0 and not args.no_stub:
        print("stub path failed, retrying with --no-stub ...")
        rc = run(build_flash_command(build_dir, port, True, args.baud), False)
    if rc == 0:
        print("Flash done. esptool hard-resets the chip; CDC + HID are back.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
