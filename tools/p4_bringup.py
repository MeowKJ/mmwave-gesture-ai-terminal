#!/usr/bin/env python3
"""Explicit UART-only headless P4 bring-up. Never writes the C5 Flash.

Unlike the product native-USB tools, this targets the user's switchable-UART
development board. Select P4 physically first; the ROM chip guard rejects C5.
Machine paths come from tools/config.toml via meoave, not this file.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import time

import meoave

PROJECT = meoave.ROOT / "firmware/p4_gesture/bringup"
BUILD = PROJECT / "build"


def hosted_result(command, text):
    """Require this command's acceptance AND terminal result, not cached READY."""
    if not re.search(rf"MEO:HOSTED_ACCEPT command={command} accepted=1\b", text):
        raise RuntimeError(f"{command} was not accepted")
    if not re.search(rf"MEO:HOSTED_{command} [^\r\n]*passed=1\b[^\r\n]*\r?\n", text):
        raise RuntimeError(f"{command} did not complete successfully; no reset/retry")


def run(command, env, report, title, cwd=PROJECT):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    report.setdefault("steps", []).append({"title": title, "exit_code": result.returncode,
                                           "output": result.stdout + result.stderr})
    print(result.stdout + result.stderr, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"{title} failed; no automatic recovery or retry")


def build(config, env, report, profile="raw"):
    digest = hashlib.sha256()
    defaults = [PROJECT / "sdkconfig.defaults"]
    if profile != "raw": defaults.append(PROJECT / "sdkconfig.hosted.defaults")
    if profile == "maintenance": defaults.append(PROJECT / "sdkconfig.maintenance.defaults")
    for name in (*defaults, PROJECT / "partitions.csv", PROJECT / "CMakeLists.txt"):
        digest.update(name.read_bytes())
    sig = json.dumps({"defaults": digest.hexdigest(), "idf": env.get("IDF_PATH"),
                      "profile": profile,
                      "python": meoave.python_command(config),
                      "compiler_bin": meoave.cfg(config,"toolchain","compiler_bin","")}, sort_keys=True)
    sigfile = BUILD / ".meoave_bringup_sig"
    BUILD.mkdir(parents=True, exist_ok=True)
    if not sigfile.exists() or sigfile.read_text(encoding="utf-8") != sig:
        # This small dedicated project's sdkconfig is generated, never a user profile.
        # Defaults changes otherwise would not override the existing sdkconfig.
        generated = BUILD / "sdkconfig" if profile != "raw" else PROJECT / "sdkconfig"
        if generated.exists():
            generated.unlink()
        run([meoave.tool_path(config,"cmake","cmake"), "-S", str(PROJECT), "-B", str(BUILD),
             "-G", "Ninja", "-DIDF_TARGET=esp32p4", f"-DPYTHON={meoave.python_command(config)}",
             f"-DMEOAVE_BRINGUP_HOSTED={'ON' if profile != 'raw' else 'OFF'}",
             f"-DSDKCONFIG={generated}", "-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults)),
             "-DPYTHON_DEPS_CHECKED=1", "-DCCACHE_ENABLE=False"], env, report, "configure P4 bring-up")
        sigfile.write_text(sig, encoding="utf-8")
    run([meoave.tool_path(config,"ninja","ninja"), "-C", str(BUILD), "-j",
         str(meoave.cfg(config,"build","jobs",8))], env, report, "build P4 bring-up")
    if profile != "raw" and f"profile={profile}".encode() not in (BUILD / "p4_c5_bringup.bin").read_bytes():
        raise RuntimeError("Hosted profile missing from image; refusing stale/raw output")


def flash(config, env, port, report):
    args = json.loads((BUILD / "flasher_args.json").read_text(encoding="utf-8"))
    if args["extra_esptool_args"]["chip"] != "esp32p4":
        raise RuntimeError("Not a P4 image")
    files = args["flash_files"]
    if {int(offset,0) for offset in files} != {0x2000, 0x8000, 0x10000}:
        raise RuntimeError("Unexpected startup/app layout")
    if report.get("profile") in ("hosted", "maintenance") and f"profile={report['profile']}".encode() not in (BUILD / "p4_c5_bringup.bin").read_bytes():
        raise RuntimeError("Hosted profile missing from image; refusing Flash write")
    report["flash_scope"] = "P4 bootloader/table/factory app only; no whole-chip erase; no C5 writes"
    pairs = [value for offset, name in files.items() for value in (offset, name)]
    run([meoave.python_command(config), "-m", "esptool", "--chip", "esp32p4", "--port", port,
         "--baud", "115200", "--no-stub", "--before", "default-reset", "--after", "hard-reset",
         "write-flash", *args["write_flash_args"], *pairs], env, report, "flash P4 ROM UART", BUILD)
    report["p4_flash_command_completed"] = True


def capture(port, seconds, commands, report):
    import serial
    import io
    from esptool.bin_image import ESP32P4FirmwareImage
    image = ESP32P4FirmwareImage(io.BytesIO((BUILD / "p4_c5_bringup.bin").read_bytes()))
    # app descriptor is at fixed offset 0x20; ELF SHA is descriptor offset 0x90.
    data = (BUILD / "p4_c5_bringup.bin").read_bytes()
    if image.checksum != image.calculate_checksum() or image.stored_digest != image.calc_digest:
        raise RuntimeError("P4 image integrity check failed")
    expected_sha = data[176:208].hex()
    report["expected_elf_sha"] = expected_sha
    raw = bytearray()
    def receive(stream, duration, terminal=None, start=0):
        deadline=time.monotonic()+duration
        while time.monotonic()<deadline:
            block=stream.read(4096)
            if block:
                raw.extend(block)
                print(block.decode("utf-8", errors="replace"), end="", flush=True)
                if terminal and re.search(terminal, raw[start:].decode("utf-8", errors="replace")):
                    break
        report["uart_text"]=raw.decode("utf-8", errors="replace")
    baud = meoave.console_uart_baud(BUILD)
    report["capture_baud"] = baud
    stream = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=3)
    stream.dtr=False; stream.rts=False; stream.port=port
    report["control_lines"] = {"dtr": False, "rts": False,
                               "note": "no toggled reset sequence during capture"}
    try:
        stream.open()
        stream.write(b"\nINFO\n"); stream.flush()
        receive(stream, 3)
        text=raw.decode("utf-8", errors="replace")
        if "project=p4_c5_bringup" not in text or f"elf_sha={expected_sha}" not in text:
            raise RuntimeError("Matching P4 firmware identity not received; refusing further commands")
        report["runtime_identity_verified"]=True
        if report.get("profile") in ("hosted", "maintenance") and f"profile={report['profile']}" not in text:
            raise RuntimeError("Matching Hosted profile not received")
        for command in commands:
            start = len(raw)
            stream.write((command+"\n").encode("ascii")); stream.flush()
            report.setdefault("commands_sent", []).append(command)
            if command in ("CONNECT", "VERIFY"):
                terminal = rf"MEO:HOSTED_{command} [^\r\n]*passed=[01][^\r\n]*\r?\n"
                receive(stream, 40 if command == "CONNECT" else 65, terminal, start)
                section = raw[start:].decode("utf-8", errors="replace")
                hosted_result(command, section)
                report[f"hosted_{command.lower()}_passed"] = True
            else:
                receive(stream, 23 if command == "C5RESET" else 17 if command == "PROBE" else 3)
        receive(stream, seconds)
        if "task_wdt:" in report["uart_text"] or "Guru Meditation" in report["uart_text"]:
            raise RuntimeError("P4 fault/watchdog observed")
    finally:
        stream.close()
        report["uart_text"] = raw.decode("utf-8", errors="replace")


def main(argv=None):
    global BUILD
    # Windows redirected consoles may default to GBK; UART noise must not abort
    # evidence capture merely because it contains a Unicode replacement glyph.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", action="store_true")
    p.add_argument("--profile", choices=("raw", "hosted", "maintenance"), default="raw")
    p.add_argument("--flash", action="store_true")
    p.add_argument("--port")
    p.add_argument("--capture", type=float, default=0)
    p.add_argument("--command", action="append", choices=("INFO","PROBE","C5RESET","STATUS","CONNECT","VERIFY"), default=[])
    p.add_argument("--report", type=Path, default=meoave.ROOT/"firmware/p4_gesture/test-results/p4_bringup.json")
    a=p.parse_args(argv)
    if not (a.build or a.flash or a.capture): p.error("select --build, --flash or --capture")
    if (a.flash or a.capture) and not a.port: p.error("explicit --port required; select P4 UART")
    if a.capture<0 or a.capture>300: p.error("--capture must be 0..300 seconds")
    if a.command and not a.capture: p.error("--command requires --capture")
    allowed = {"INFO", "STATUS", "CONNECT", "VERIFY"} if a.profile == "hosted" else {"INFO"} if a.profile == "maintenance" else {"INFO", "PROBE", "C5RESET"}
    if any(cmd not in allowed for cmd in a.command): p.error("command unavailable in selected SDIO owner profile")
    BUILD = PROJECT / ("build" if a.profile == "raw" else "build_" + a.profile)
    r={"ok":False,"chip":"esp32p4","port":a.port,"c5_flash_written":False,
       "profile":a.profile,
       "scope":"new development board UART bring-up; not product UI/OTA acceptance"}
    try:
        config=meoave.load_config(); env=meoave.environment(config)
        env["ESP_ROM_ELF_DIR"]=meoave.esp_rom_elf_dir(config)
        if a.build: build(config,env,r,a.profile)
        if a.flash: flash(config,env,a.port,r)
        if a.capture: capture(a.port,a.capture,a.command,r)
        r["ok"]=True
        return 0
    except Exception as exc:
        r["error"]=f"{type(exc).__name__}: {exc}"
        print(r["error"], flush=True)
        return 1
    finally:
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(r,ensure_ascii=False,indent=2),encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
