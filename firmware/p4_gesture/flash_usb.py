#!/usr/bin/env python3
"""Flash an already-powered board over native USB, preserving GPIO29 hold.

Hardware: ESP32P4C5 module pins 48/49 are the P4 USB 2.0 High-Speed OTG
dedicated USB_DM/USB_DP pads. The same pair is used in two states:

  * application : TinyUSB composite CDC-ACM + HID, VID 0x303A / PID 0x4005
  * ROM download: Espressif USB-OTG CDC,       VID 0x303A / PID 0x0012
                  (0x0012 = esptool ESP32P4ROM.IMAGE_CHIP_ID = 18)

This script:
  1. finds the application CDC port,
  2. writes ``MEOAVE-DOWNLOAD\\n`` to it; the firmware disconnects USB, sets
     LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT and calls esp_restart(),
  3. waits for the ROM download device (PID 0x0012) to enumerate,
  4. writes/verifies in ROM with --no-stub --after no-reset,
  5. clears FORCE_DOWNLOAD_BOOT and resets only the two HP CPUs,
  6. verifies the running ELF SHA, fresh nonce, boot ID and power hold.

Never uses the chip-level watchdog or ROM FLASH_END soft-reset path. A cold
power-on still needs SW1. No UART, DTR/RTS pulse, or automatic reflash fallback.

Usage:  python flash_usb.py [--port COMx] [--no-stub] [--dry-run]
"""

import argparse
import json
import hashlib
import re
import secrets
import struct
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
    return [p for p in list_ports.comports()
            if p.vid == ESP_VID and p.pid in APP_PIDS | ROM_PIDS]


def select_port(pids, port_name=None):
    ports = [p for p in espressif_ports() if p.pid in pids]
    if port_name:
        ports = [p for p in ports if p.device.casefold() == port_name.casefold()]
        if not ports:
            raise RuntimeError(f"Refusing non-native or wrong-mode USB port: {port_name}")
    if len(ports) > 1:
        raise RuntimeError("Multiple native USB targets; disconnect the other target")
    return ports[0] if ports else None


def rom_port():
    return select_port(ROM_PIDS)


def app_port():
    return select_port(APP_PIDS)


def open_native(port_name, pids, timeout=0.1):
    select_port(pids, port_name)  # Validate VID/PID before opening anything.
    stream = serial.Serial(port=None, baudrate=115200, timeout=timeout, write_timeout=2)
    stream.dtr = stream.rts = True  # Set before open, avoiding a reset pulse.
    stream.port = port_name
    stream.open()
    return stream


def describe(p):
    return f"{p.device} VID={p.vid:04X} PID={p.pid:04X} {p.product or ''}"


def send_magic(port_name):
    """Primary trigger: an explicit CDC command, immune to Windows usbser.sys
    coalescing SET_CONTROL_LINE_STATE requests."""
    with open_native(port_name, APP_PIDS) as s:
        time.sleep(0.05)
        s.write(MAGIC)
        s.flush()
        time.sleep(0.15)


def send_reset_pulse(port_name):
    raise RuntimeError("DTR/RTS fallback disabled; use native USB CDC magic")


def wait_for_rom(timeout=12.0):
    deadline = time.monotonic() + timeout
    saw_disconnect = False
    while time.monotonic() < deadline:
        p = rom_port()
        if p:
            return p, saw_disconnect
        if not app_port():
            saw_disconnect = True
        time.sleep(0.25)
    return None, saw_disconnect


def wait_for_app(timeout=20.0):
    """Wait for exact native application PID, not an arbitrary serial device."""
    deadline = time.monotonic() + timeout
    rom_gone = False
    while time.monotonic() < deadline:
        p = app_port()
        if p:
            return p, rom_gone
        if not rom_port():
            rom_gone = True
        time.sleep(0.25)
    return None, rom_gone


def build_flash_command(build_dir, port, no_stub, baud="921600"):
    args_file = Path(build_dir) / "flasher_args.json"
    if not args_file.is_file():
        raise RuntimeError(f"{args_file} not found - build the project first")
    data = json.loads(args_file.read_text(encoding="utf-8"))
    settings = data.get("flash_settings", {})

    cmd = [sys.executable, "-m", "esptool",
           "--chip", "esp32p4", "-p", port, "-b", baud,
           "--before", "no-reset", "--after", "no-reset", "--no-stub"]
    # no_stub is retained for older callers. This board's safe path always
    # uses ROM, avoiding the stub's FLASH_END/reset implementation as well.
    cmd.append("write-flash")
    cmd.append("--no-progress")
    # esptool defaults to uncompressed transfer when --no-stub is used.
    # ROM deflate keeps the same reset-safe path and bounds large payload time.
    cmd.append("--compress")
    for key, flag in (("flash_mode", "--flash-mode"),
                      ("flash_size", "--flash-size"),
                      ("flash_freq", "--flash-freq")):
        if settings.get(key):
            cmd += [flag, str(settings[key])]

    files = sorted(data.get("flash_files", {}).items(), key=lambda kv: int(kv[0], 0))
    if not files:
        raise RuntimeError("no flash_files in flasher_args.json")
    for offset, rel in files:
        path = Path(rel)
        if not path.is_absolute():
            path = Path(build_dir) / path
        if not path.is_file():
            raise RuntimeError(f"missing image: {path}")
        cmd += [offset, str(path)]
    return cmd


def run(cmd, dry_run):
    print("  " + " ".join(str(c) for c in cmd))
    if dry_run:
        return 0
    return subprocess.call([str(c) for c in cmd])


def write_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


def image_identity(build_dir):
    """Read the ESP-IDF app descriptor, not the whole-bin SHA as an ELF SHA.

    Image header = 24 bytes, first segment header = 8; app descriptor magic
    at +0, app_elf_sha256 at +144 (esp_app_desc.h, IDF 6.1).
    """
    build_dir = Path(build_dir).resolve()
    manifest = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    app = manifest["app"]
    path = Path(app["file"])
    if not path.is_absolute():
        path = build_dir / path
    data = path.read_bytes()
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise RuntimeError("Invalid ESP-IDF app image/descriptor")
    sha = data[176:208].hex()
    if sha == "00" * 32:
        raise RuntimeError("Image has no ELF SHA; refusing unverifiable update")
    return {"path": str(path), "elf_sha": sha,
            "app_size": len(data),
            "bin_sha256": hashlib.sha256(data).hexdigest(),
            "image_count": len(manifest["flash_files"])}


INFO_PATTERN = re.compile(
    r"^SYS:INFO token=([a-zA-Z0-9]+) sha=([0-9a-f]{64}) reset=(\d+) "
    r"uptime_us=(\d+) boot=([0-9a-f]{8})$")
POWER_PATTERN = re.compile(
    r"^SYS:POWER token=([a-zA-Z0-9]+) hold=([01]) ready=([01]) key=([01]) marker=([01])$")
FAULT_PATTERN = re.compile(
    r"^SYS:FAULT token=([a-zA-Z0-9]+) valid=([01]) core=(\d+) pc=([0-9a-f]{8}) "
    r"ra=([0-9a-f]{8}) sp=([0-9a-f]{8}) cause=([0-9a-f]{8}) bad=([0-9a-f]{8})$")


def query(stream, command, pattern, timeout=3.0):
    token = secrets.token_hex(6)
    deadline = time.monotonic() + timeout
    pending = b""
    next_send = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            stream.write(f"SYS {command} {token}\n".encode("ascii"))
            stream.flush()
            next_send = now + 0.6
        pending += stream.read(4096)
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            match = pattern.fullmatch(line.decode("ascii", errors="replace").rstrip("\r"))
            if match and match[1] == token:
                return match.groups()
        pending = pending[-4096:]  # Bounded even with malformed/no-newline input.
    raise TimeoutError(f"No complete fresh SYS {command} reply")


def check_application(expected_sha, timeout=30.0, previous_boot=None):
    """Enumeration is insufficient: require fresh identity and high held power."""
    deadline = time.monotonic() + timeout
    last_error = "application USB absent"
    while time.monotonic() < deadline:
        p = app_port()
        if p:
            try:
                with open_native(p.device, APP_PIDS) as stream:
                    info = query(stream, "INFO", INFO_PATTERN,
                                 min(2.0, max(0.1, deadline - time.monotonic())))
                    if info[1] != expected_sha:
                        raise RuntimeError(f"Running SHA mismatch: {info[1]} != {expected_sha}")
                    if previous_boot is not None and info[4] == previous_boot:
                        last_error = "old boot still running"
                        time.sleep(0.15)
                        continue
                    power = query(stream, "POWER", POWER_PATTERN,
                                  min(2.0, max(0.1, deadline - time.monotonic())))
                    if power[1:4] != ("1", "1", "0"):
                        raise RuntimeError("Power hold not high/latched, or SW1 is held down")
                    return {"port": p.device, "elf_sha": info[1], "reset_reason": int(info[2]),
                            "uptime_us": int(info[3]), "boot_id": info[4],
                            "pad_hold": True, "power_ready": True, "key_pressed": False,
                            "marker_retained": power[4] == "1"}
            except (serial.SerialException, OSError, TimeoutError) as exc:
                last_error = str(exc)
        time.sleep(0.2)
    raise TimeoutError(f"Application recovery not verified: {last_error}")


def read_fault_diagnostic(port_name):
    try:
        with open_native(port_name, APP_PIDS) as stream:
            fields = query(stream, "FAULT", FAULT_PATTERN, timeout=2.5)
        return dict(zip(("valid", "core", "pc", "ra", "sp", "cause", "badaddr"), fields[1:]))
    except (serial.SerialException, OSError, TimeoutError) as exc:
        return {"unavailable": str(exc)}


def require_clean_reset(application, report):
    if application["reset_reason"] != 3 or not application["marker_retained"]:
        report["fault"] = read_fault_diagnostic(application["port"])
        raise RuntimeError(f"Program restart reset reason/retained marker not verified: {application}")


def cpu_reset_from_rom(port_name):
    """ESP32-P4 rev3 only: preserve the LP domain and GPIO29 pad latch.

    IDF 6.1 hw_ver3: LP_CLKRST.HPCPU_RESET_CTRL0 bits13/29 are the two
    software CPU reset pulses (same register used by cpu_utility_ll_reset_cpu).
    ROM FLASH_END and esptool's chip watchdog reset are deliberately not used.
    Only the final reset write may lose its USB reply; that is not itself PASS.
    """
    from esptool.targets.esp32p4 import ESP32P4ROM
    from esptool.util import FatalError

    with open_native(port_name, ROM_PIDS, timeout=1.0) as stream:
        esp = ESP32P4ROM(stream, 115200)
        esp.connect(mode="no-reset", attempts=3)
        revision = esp.get_chip_revision()
        if not 300 <= revision < 400 or esp.sync_stub_detected:
            raise RuntimeError("Guarded reset requires P4 rev3 ROM, not a stub/other revision")
        addresses = {"pad_hold": 0x501101C4, "gpio_out": 0x500E0004,
                     "gpio_enable": 0x500E0020, "boot_flags": 0x50110008,
                     "cpu_reset_ctrl": 0x50111014}
        registers = {key: esp.read_reg(addr) for key, addr in addresses.items()}
        if not all(registers[key] & (1 << 29)
                   for key in ("pad_hold", "gpio_out", "gpio_enable")):
            raise RuntimeError("GPIO29 is not high/output/held; refusing reset")
        esp.write_reg(addresses["boot_flags"], registers["boot_flags"] & ~4)
        if esp.read_reg(addresses["boot_flags"]) & 4:
            raise RuntimeError("FORCE_DOWNLOAD_BOOT failed to clear; refusing reset")
        # Recheck hold immediately before the only reset-triggering write.
        if not esp.read_reg(addresses["pad_hold"]) & (1 << 29):
            raise RuntimeError("GPIO29 pad hold changed; refusing reset")
        result = {"revision": revision, "registers_before": registers,
                  "kind": "HP_CPU_ONLY", "transport_note": None}
        try:
            esp.write_reg(addresses["cpu_reset_ctrl"],
                          registers["cpu_reset_ctrl"] | (1 << 13) | (1 << 29))
        except (serial.SerialException, OSError, FatalError) as exc:
            result["transport_note"] = f"{type(exc).__name__}: {exc}"
        return result


def flash_once(build_dir, port_hint=None, baud="921600", timeout=15.0,
               app_timeout=30.0, report=None):
    if report is None:
        report = {}
    report.update(ok=False, stage="preflight", reset_kind="HP_CPU_ONLY")
    identity = image_identity(build_dir)
    report["image"] = identity
    build_flash_command(build_dir, "COMx", True, baud)  # Check ALL images first.
    ports = espressif_ports()
    if len(ports) != 1:
        raise RuntimeError("Exactly one powered native USB target is required; no UART fallback")
    initial = select_port(APP_PIDS | ROM_PIDS, port_hint)
    if initial is None:
        raise RuntimeError("Native USB target absent (cold power-on still requires SW1)")
    report["initial_port"] = initial.device
    report["stage"] = "request_download"
    if initial.pid in APP_PIDS:
        try:
            send_magic(initial.device)
        except (serial.SerialException, OSError) as exc:
            # A disconnect while sending the magic is resolved by checking ROM,
            # not by blindly resetting or reflashing a second time.
            report["download_transport_note"] = str(exc)
        rom, _ = wait_for_rom(timeout)
        if rom is None:
            raise TimeoutError("ROM USB absent after CDC magic; no reset pulse fallback")
    else:
        rom = initial
    report["rom_port"] = rom.device
    time.sleep(0.5)
    report["stage"] = "flash"
    command = build_flash_command(build_dir, rom.device, True, baud)
    print("  " + " ".join(command), flush=True)
    try:
        flash_timeout = max(180, 60 + identity.get("app_size", 0) // 8192)
        report["flash_timeout_seconds"] = flash_timeout
        written = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=flash_timeout)
    except subprocess.TimeoutExpired as exc:
        # Keep partial esptool evidence: a timeout is neither a verified write
        # nor permission to reset/reflash. The caller can inspect/verify ROM.
        def decoded(value):
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else (value or "")
        report["flash_output"] = decoded(exc.stdout) + decoded(exc.stderr)
        report["flash_timeout"] = True
        print(report["flash_output"], flush=True)
        raise
    output = written.stdout + written.stderr
    print(output, flush=True)
    report["flash_output"] = output
    report["flash_returncode"] = written.returncode
    report["verified_images"] = output.count("Hash of data verified")
    if written.returncode != 0 or report["verified_images"] != identity["image_count"]:
        raise RuntimeError("Flash/write verification failed; no automatic reflash or hard reset")
    report["flash_verified"] = True
    report["stage"] = "program_reset"
    rom = rom_port()
    if rom is None:
        raise RuntimeError("Verified flash but ROM USB missing; do not reflash")
    report["reset"] = cpu_reset_from_rom(rom.device)
    report["stage"] = "application_verification"
    report["application"] = check_application(identity["elf_sha"], app_timeout)
    require_clean_reset(report["application"], report)
    report.update(ok=True, stage="complete")
    print("PASS: images verified, CPU-only reset, running SHA + fresh USB + power hold", flush=True)
    return report


def restart_once(expected_sha, timeout=30.0, report=None):
    report = report if report is not None else {}
    report.update(ok=False, stage="preflight")
    before = check_application(expected_sha, timeout)
    report["before"] = before
    report["stage"] = "request_restart"
    with open_native(before["port"], APP_PIDS) as stream:
        token = secrets.token_hex(6)
        stream.write(f"SYS RESTART {token}\n".encode("ascii"))
        stream.flush()
    report["stage"] = "application_verification"
    after = check_application(expected_sha, timeout, previous_boot=before["boot_id"])
    report["after"] = after
    require_clean_reset(after, report)
    report.update(ok=True, stage="complete")
    return report


def check_fls2_runtime(port_name, timeout=12.0, report=None):
    """Read-only health for the current FLS2 profile; not a visual PASS.

    No display commands, scans, DCS reads, loop pause or synthetic touch.
    TOUCH ID requests a serialized read-only 0xA7 refresh, not a cached boot ID.
    Require the actual touch-driver counter and PONG uptime to advance.
    """
    deadline = time.monotonic() + timeout
    report = report if report is not None else {}
    report.update(ok=False, visual="NOT_RETESTED")
    pending = b""
    samples, pongs, lines = [], [], []
    report["lines"] = lines
    ready = touch_id = False
    next_send = 0.0
    with open_native(port_name, APP_PIDS) as stream:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                stream.write(b"PING\nSTATUS\nTOUCH ID\nTOUCH\n")
                stream.flush()
                next_send = now + 0.8
            pending += stream.read(4096)
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("ascii", errors="replace").rstrip("\r")
                lines.append(line)
                if re.fullmatch(r"FLS2:STATUS profile=\S+ ready=1 usb=1 .+", line):
                    ready = True
                if re.fullmatch(r"FLS2:TOUCH init=1 .+ id=0xB7 id_ok=\d+ id_err=0", line):
                    touch_id = True
                match = re.fullmatch(r"FLS2:PONG uptime_us=(\d+)", line)
                if match:
                    pongs.append(int(match[1]))
                match = re.fullmatch(r"FLS2:TOUCH_IO driver_ok=(\d+) driver_err=(\d+) .+", line)
                if match:
                    if int(match[2]) != 0:
                        raise RuntimeError("Actual touch driver reported communication errors")
                    samples.append(int(match[1]))
            pending = pending[-4096:]
            if (ready and touch_id and len(samples) > 1 and samples[-1] > samples[0]
                    and len(pongs) > 1 and pongs[-1] > pongs[0]):
                report.update({"ok": True, "ready": True, "touch_id": "0xB7",
                        "driver_ok_first": samples[0], "driver_ok_last": samples[-1],
                        "driver_err": 0, "pong_first_us": pongs[0], "pong_last_us": pongs[-1],
                        "visual": "NOT_RETESTED", "lines": lines})
                return report
    raise TimeoutError("FLS2 runtime ready/touch/PONG progress not fully verified")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="force the application CDC port")
    ap.add_argument("--build-dir", default="build_fls2_official")
    ap.add_argument("--baud", default="921600")
    ap.add_argument("--no-stub", action="store_true", help="compatibility option; ROM-only is always used")
    ap.add_argument("--dry-run", action="store_true", help="print the esptool command only")
    ap.add_argument("--timeout", type=float, default=12.0)
    ap.add_argument("--app-timeout", type=float, default=30.0)
    ap.add_argument("--report", default="test-results/usb_flash.json")
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

    report = {"ok": False}
    try:
        flash_once(build_dir, args.port, args.baud, args.timeout, args.app_timeout, report)
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print("FAIL: " + report["error"])
        return 1
    finally:
        write_report(args.report, report)


if __name__ == "__main__":
    sys.exit(main())
