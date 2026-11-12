#!/usr/bin/env python3
"""Catch a hand-strapped C5 download boot through P4 native USB + SDIO.

The module's download straps (GPIO26/27/28) are not routed to the P4, so the
only way to land a shaky-hands strap is to reset the C5 on a fixed period and
probe every window. `watch` asks the programmer profile to do exactly that:
one shared GPIO54 EN pulse, then a short SDIO download probe, once per second,
bounded by --attempts. It stops on the first hit.

A hit only means the ROM download target answered. Nothing is written unless
--flash is passed; then the four embedded regions are erased/written/MD5
verified in the same window. No strap, GPIO6 or COM3 operation happens here.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware/p4_gesture"))
sys.path.insert(0, str(ROOT / "tools"))
import flash_usb as usb
import c5_program as prog

DEFAULT_BUILD = ROOT / "firmware/p4_gesture/build_meoave_programmer"
WATCH = re.compile(
    r'MEO:C5_WATCH token=(?P<token>[a-zA-Z0-9]{1,16}) operation=(?P<operation>[a-zA-Z0-9]{1,16}) '
    r'state=(?P<state>IDLE|QUEUED|WATCHING|HIT|EXHAUSTED|FAILED) stage=(?P<stage>[A-Z0-9_]+) '
    r'error=(?P<error>\d+) attempt=(?P<attempt>\d+) max=(?P<max>\d+) lows=(?P<lows>\d+)')
WATCH_ACK = re.compile(r'MEO:C5_WATCH_ACCEPT token=([a-zA-Z0-9]{1,16}) accepted=([01])')


def payload_path(build_dir):
    """Use the payload the flashed P4 image actually embeds."""
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    for line in cache.splitlines():
        if line.startswith("C5_PAYLOAD_FILE:UNINITIALIZED=") or line.startswith("C5_PAYLOAD_FILE:PATH="):
            return Path(line.split("=", 1)[1])
    raise RuntimeError("Build directory has no embedded C5 payload")


def expected_total(payload):
    manifest = json.loads((payload.parent / "flasher_args.json").read_text(encoding="utf-8"))
    if manifest.get("extra_esptool_args", {}).get("chip") != "esp32c5":
        raise ValueError("Not an ESP32-C5 payload")
    return sum((payload.parent / name).stat().st_size for name in manifest["flash_files"].values())


def send(stream, command):
    stream.write(f"C5 {command}\n".encode("ascii"))
    stream.flush()


def watch(stream, token, attempts, period, report, log):
    """One bounded burst. Returns the final record; never resends a pulse."""
    deadline = time.monotonic() + attempts * period + 90
    pending = b""
    send(stream, f"WATCH {token} {attempts}")
    accepted = None
    last = None
    last_printed = None
    while time.monotonic() < deadline:
        pending += stream.read(4096)
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode("ascii", errors="replace").rstrip("\r")
            if len(report["lines"]) < 4000:
                report["lines"].append(line)
            ack = WATCH_ACK.fullmatch(line)
            if ack and ack[1] == token:
                accepted = ack[2] == "1"
                if not accepted:
                    raise RuntimeError("WATCH rejected; programmer not idle; no automatic retry")
                log("窗口已开：每 %0.1fs 复位一次 C5，共 %d 次。现在开始拉低三个引脚。" % (period, attempts))
                continue
            m = WATCH.fullmatch(line)
            if not m or m["token"] != token:
                continue
            last = {k: (v if k in ("token", "operation", "state", "stage") else int(v))
                    for k, v in m.groupdict().items()}
            report["last_watch"] = last
            if last["state"] == "WATCHING":
                if last["attempt"] != last_printed:
                    last_printed = last["attempt"]
                    if last["attempt"] % 5 == 1 or last["lows"] == 0:
                        log("第 %d/%d 次：%s，error=%d，低电平采样=%d"
                            % (last["attempt"], last["max"], last["stage"],
                               last["error"], last["lows"]))
            elif last["state"] == "HIT":
                log("命中：第 %d 次窗口出现 SDIO 下载目标（error=%d）。"
                    % (last["attempt"], last["error"]))
                return last
            elif last["state"] in ("EXHAUSTED", "FAILED"):
                log("结束：%s，最后一次 error=%d。" % (last["state"], last["error"]))
                return last
        if len(pending) > 8192:
            raise ValueError("Unbounded programmer output")
    if accepted is None:
        raise TimeoutError("WATCH was never acknowledged")
    raise TimeoutError("WATCH burst did not finish; no second burst, no reset")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    p.add_argument("--port", help="native application USB only; auto-detect if omitted")
    p.add_argument("--attempts", type=int, default=60, help="1..240 shared-line EN pulses")
    p.add_argument("--period", type=float, default=1.0, help="seconds between pulses (>=0.4)")
    p.add_argument("--start-delay", type=float, default=10.0,
                   help="seconds to get hands on the pins after the window opens")
    p.add_argument("--flash", action="store_true",
                   help="on a hit, erase/write/verify the four embedded regions")
    p.add_argument("--install", action="store_true", help="flash the P4 programmer image first")
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args(argv)
    if not 1 <= a.attempts <= 240:
        p.error("--attempts must be 1..240")
    if a.period < 0.4:
        p.error("--period below 0.4s cannot complete one probe window")
    if a.report.exists():
        p.error("Report exists; use a fresh evidence filename")

    def log(text):
        print(text, flush=True)

    r = dict(ok=False, started=datetime.now().astimezone().isoformat(), lines=[],
             scope="custom-board native USB/P4/SDIO; manual straps; not screen acceptance",
             c5_reset="repeated_en_pulses", c5_firmware_write_requested=bool(a.flash),
             c5_straps_changed=False)
    try:
        payload = payload_path(a.build_dir)
        r["payload"] = str(payload)
        sha = hashlib.sha256(payload.read_bytes()).hexdigest()
        r["payload_sha256"] = sha
        import meoave
        meoave.programmer_payload_layout(payload)   # reject stale region content
        total = r["expected_total"] = expected_total(payload)
        info = usb.image_identity(a.build_dir)
        if a.install:
            usb.flash_once(a.build_dir, report=r.setdefault("p4_install", {}))
        before = r["before"] = usb.check_application(info["elf_sha"], 15)
        if a.port and a.port.casefold() != before["port"].casefold():
            raise RuntimeError("Selected port is not the verified native P4")
        with usb.open_native(before["port"], usb.APP_PIDS) as stream:
            initial = prog.exchange(stream, None, sha, r, 5)
            if initial["state"] != "IDLE":
                raise RuntimeError("Programmer is not idle; no hidden reset or retry")
            log("P4 已就绪。%d 秒后开始抓窗口，请把手指放到模组 6/7/8 脚上。"
                % round(a.start_delay))
            time.sleep(max(a.start_delay, 0))
            token = secrets.token_hex(6)
            hit = watch(stream, token, a.attempts, a.period, r, log)
            r["hit"] = hit
            if hit["state"] == "HIT":
                r["facts"] = prog.read_facts(stream, r)
                if a.flash:
                    log("开始写入：bootloader + 分区表 + otadata + 应用，逐段校验。")
                    done = prog.exchange(stream, "FLASH", sha, r, 300)
                    if done["total"] != total or not done["verified"]:
                        raise RuntimeError("Flash finished without full verified length")
                    r["flash"] = done
                    log("写入完成：%d/%d 字节，MD5 校验通过。" % (done["written"], done["total"]))
            if hit["state"] != "HIT" and a.flash:
                r["flash_skipped"] = "no download window"
        after = r["after"] = usb.check_application(info["elf_sha"], 10)
        if before["boot_id"] != after["boot_id"] or after["uptime_us"] <= before["uptime_us"]:
            raise RuntimeError("P4 reset/stall during the C5 window")
        r["fault"] = usb.read_fault_diagnostic(after["port"])
        if r["fault"].get("valid") != "0":
            raise RuntimeError("P4 fault diagnostic is not clear")
        r["ok"] = r.get("hit", {}).get("state") == "HIT"
        if not r["ok"]:
            print("结果：窗口内没有出现 SDIO 下载目标；没有写 Flash", flush=True)
        elif a.flash:
            print("PASS: 抓到下载窗口，固件已写入并校验", flush=True)
        else:
            print("PASS: 抓到下载窗口，未写 Flash", flush=True)
        return 0 if r["ok"] else 2
    except Exception as exc:
        r["error"] = f"{type(exc).__name__}: {exc}"
        log("失败：" + r["error"])
        return 1
    finally:
        r["finished"] = datetime.now().astimezone().isoformat()
        usb.write_report(a.report, r)


if __name__ == "__main__":
    raise SystemExit(main())
