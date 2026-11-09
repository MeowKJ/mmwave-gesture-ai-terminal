#!/usr/bin/env python3
"""Bounded native-USB build/flash/restart acceptance, never accesses UART.

Run with the ESP-IDF Python environment. Requires an already-powered board.
Example: python usb_iterate.py --build --cycles 3 --restarts 3
"""
import argparse
from datetime import datetime
from pathlib import Path
import subprocess
import sys

import flash_usb as usb

HERE = Path(__file__).resolve().parent


def verify_runtime(cycle, application, expected, timeout):
    cycle.update(ok=False, stage="runtime_verification")
    cycle["runtime"] = {}
    try:
        usb.check_fls2_runtime(application["port"], report=cycle["runtime"])
    except Exception:
        try:
            current = usb.check_application(expected, min(timeout, 5.0))
            cycle["application_after_runtime"] = current
            cycle["fault"] = usb.read_fault_diagnostic(current["port"])
        except Exception as exc:
            cycle["runtime_diagnostic_error"] = str(exc)
        raise
    final = usb.check_application(expected, timeout)
    cycle["application_after_runtime"] = final
    usb.require_clean_reset(final, cycle)
    if final["boot_id"] != application["boot_id"]:
        raise RuntimeError("Unexpected additional reboot during runtime verification")
    cycle.update(ok=True, stage="complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build_fls2_official")
    parser.add_argument("--build", action="store_true", help="run ninja before hardware tests")
    parser.add_argument("--cycles", type=int, default=1, help="complete flash cycles; 0 allows restart-only tests")
    parser.add_argument("--restarts", type=int, default=3, help="additional application esp_restart tests")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--port", help="initial exact native USB port; COM3/UART is refused")
    parser.add_argument("--baud", default="921600", help="ROM transfer rate (native USB CDC)")
    parser.add_argument("--report", default=str(HERE / "test-results/usb_iteration.json"))
    args = parser.parse_args()
    if (not 0 <= args.cycles <= 20 or not 0 <= args.restarts <= 50
            or args.cycles + args.restarts == 0 or not 1 <= args.timeout <= 120):
        parser.error("cycles: 0..20, restarts: 0..50 (at least one test), timeout: 1..120 seconds")
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = HERE / build_dir
    report = {"ok": False, "started": datetime.now().astimezone().isoformat(),
              "flash_cycles": [], "program_restarts": [],
              "scope": "already-powered P4 rev3, native USB only; no physical voltage measurement"}
    try:
        if args.build:
            subprocess.run(["ninja", "-C", str(build_dir), "-j", "12"], check=True, timeout=600)
        expected = usb.image_identity(build_dir)["elf_sha"]
        for i in range(args.cycles):
            cycle = {"number": i + 1}
            report["flash_cycles"].append(cycle)
            usb.write_report(args.report, report)
            print(f"\n=== FLASH CYCLE {i + 1}/{args.cycles} ===", flush=True)
            usb.flash_once(build_dir, args.port if i == 0 else None, baud=args.baud,
                           app_timeout=args.timeout, report=cycle)
            verify_runtime(cycle, cycle["application"], expected, args.timeout)
            usb.write_report(args.report, report)
        for i in range(args.restarts):
            print(f"\n=== PROGRAM RESTART {i + 1}/{args.restarts} ===", flush=True)
            cycle = {"number": i + 1, "ok": False}
            report["program_restarts"].append(cycle)
            usb.restart_once(expected, args.timeout, report=cycle)
            verify_runtime(cycle, cycle["after"], expected, args.timeout)
            usb.write_report(args.report, report)
        report["ok"] = True
        print(f"PASS: {args.cycles} verified downloads + {args.restarts} program restarts; "
              "fresh firmware identity and SW1 released verified", flush=True)
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print("FAIL: " + report["error"], file=sys.stderr, flush=True)
        return 1
    finally:
        report["finished"] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == "__main__":
    sys.exit(main())
