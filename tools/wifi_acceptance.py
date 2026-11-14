#!/usr/bin/env python3
"""Product Wi-Fi scan acceptance via native USB. No flash, C5 reset or JOIN.

Every SCAN is sent once. Polls only read cached status. Requires exact linked
ELF identity, held power, clear fault record, fresh scan generation and APs.
Software/radio acceptance does not claim visual/touch or Internet acceptance.
"""
import argparse
import json
from pathlib import Path
import re
import secrets
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware/p4_gesture"))
import flash_usb as usb


class Link:
    def __init__(self, stream):
        self.stream = stream
        self.pending = b""

    def command(self, verb, prefix, argument="", timeout=3):
        token = secrets.token_hex(6)
        self.stream.write(f"WIFI {verb} {token}{' ' + argument if argument else ''}\n".encode("ascii"))
        self.stream.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pending += self.stream.read(4096)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if not line.startswith(prefix + " "):
                    continue
                fields = dict(re.findall(r"(?:^|\s)([a-z_]+)=([^\s]+)", line))
                if fields.get("token") == token:
                    return fields
            self.pending = self.pending[-8192:]
        raise TimeoutError(f"No complete fresh {verb} reply; not retransmitting")


def completed_scan(status, seq):
    if int(status.get("scan_seq", -1)) != seq:
        raise RuntimeError("Scan generation changed (another UI/client requested a scan)")
    if int(status.get("scan_done", -1)) != seq or status.get("scanning") != "0":
        return False
    if int(status.get("scan_error", -1)) != 0:
        raise RuntimeError("Real scan failed: " + str(status))
    return True


def run(stream, expected_sha, cycles, report):
    before = usb.query(stream, "INFO", usb.INFO_PATTERN)
    if before[1] != expected_sha:
        raise RuntimeError("Running P4 ELF does not match the selected product build; no scan sent")
    power = usb.query(stream, "POWER", usb.POWER_PATTERN)
    fault = usb.query(stream, "FAULT", usb.FAULT_PATTERN)
    if power[1:3] != ("1", "1") or fault[1] != "0":
        raise RuntimeError("Power/fault guard failed; no scan sent")
    report.update(boot=before[4], elf_sha=before[1], scans=[], reset_c5=False, credentials_sent=False)
    link = Link(stream)
    deadline = time.monotonic() + 40
    while True:
        state = link.command("STATUS", "MEO:WIFI")
        if state.get("state") == "ERROR" and state.get("radio") != "1":
            raise RuntimeError("Product radio failed; not resetting C5: " + str(state))
        if state.get("state") not in ("OFF", "STARTING", "JOINING", "SCANNING") and state.get("scanning") == "0":
            break
        if time.monotonic() > deadline:
            raise TimeoutError("Radio did not become idle; not resetting C5")
        time.sleep(.5)
    for _ in range(cycles):
        seq = int(state["scan_seq"]) + 1
        accepted = link.command("SCAN", "MEO:WIFI_ACCEPT")
        if accepted.get("queued") != "1":
            raise RuntimeError("SCAN rejected: " + str(accepted))
        row = {"seq": seq, "accepted": True, "completed": False}
        report["scans"].append(row)
        deadline = time.monotonic() + 26
        while True:
            state = link.command("STATUS", "MEO:WIFI")
            if completed_scan(state, seq):
                break
            if time.monotonic() > deadline:
                raise TimeoutError("No fresh scan completion; not retransmitting SCAN")
            time.sleep(.5)
        summary = link.command("APS", "MEO:WIFI_SCAN")
        if int(summary["done"]) != seq or summary.get("scanning") != "0" or int(summary["error"]):
            raise RuntimeError("AP snapshot does not belong to the completed scan")
        count = int(summary["count"])
        if not 0 <= count <= 6:
            raise RuntimeError("Invalid AP count")
        aps = []
        for index in range(count):
            ap = link.command("AP", "MEO:WIFI_AP", str(index))
            if int(ap["seq"]) != seq or int(ap["index"]) != index:
                raise RuntimeError("Stale/wrong AP record")
            name = bytes.fromhex(ap["ssid_hex"])
            signal = int(ap["rssi"])
            if not 1 <= len(name) <= 32 or not -127 <= signal <= 0:
                raise RuntimeError("Invalid AP SSID/RSSI")
            aps.append(dict(ssid_hex=ap["ssid_hex"], ssid=name.decode("utf-8", errors="replace"), rssi=signal))
        row.update(completed=True, raw=int(state["raw"]), aps=aps)
        print(f"Scan {seq}: driver={state['raw']}, visible={count}", flush=True)
    after = usb.query(stream, "INFO", usb.INFO_PATTERN)
    power = usb.query(stream, "POWER", usb.POWER_PATTERN)
    fault = usb.query(stream, "FAULT", usb.FAULT_PATTERN)
    if after[1] != before[1] or after[4] != before[4] or power[1:3] != ("1", "1") or fault[1] != "0":
        raise RuntimeError("P4 identity/boot/power/fault changed during scans")
    report["protocol_ok"] = True
    report["ok"] = any(row["aps"] for row in report["scans"])
    report["visual"] = "NOT_TESTED"
    report["internet"] = "NOT_TESTED"
    if not report["ok"]:
        report["error"] = "All real scans completed but found no visible APs; reception not accepted"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "firmware/p4_gesture/build_meoave")
    parser.add_argument("--cycles", type=int, default=3, choices=range(1, 11))
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    # Reserve before opening hardware; do not overwrite another acceptance run.
    with args.report.open("x", encoding="utf-8") as output:
        report = {"ok": False, "protocol_ok": False, "reset_c5": False}
        try:
            import meoave
            meoave.verify_product_wifi(args.build_dir)
            expected = usb.image_identity(args.build_dir)
            port = usb.select_port(usb.APP_PIDS, args.port)
            if not port:
                raise RuntimeError("Native application USB absent; no flash/reset attempted")
            with usb.open_native(port.device, usb.APP_PIDS) as stream:
                run(stream, expected["elf_sha"], args.cycles, report)
        except Exception as exc:
            report["error"] = f"{type(exc).__name__}: {exc}"
            print(report["error"], flush=True)
        finally:
            json.dump(report, output, indent=2, ensure_ascii=False)
            output.write("\n")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
