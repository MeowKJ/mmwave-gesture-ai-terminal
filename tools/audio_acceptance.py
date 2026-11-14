#!/usr/bin/env python3
"""Bounded product-audio acceptance over native USB, with a saved transcript.

Does not flash, reboot, calibrate inputs, write arbitrary registers or access C5.
Plays the UI's short melody at 30% and 0%, plus a 400 ms acoustic self-test at
the firmware's quiet test level. On success, leaves volume at 30% and returns
to the eyes. On failure, stops without hidden reset/recovery or output retry.
Serial/software success is not a subjective sound-quality or touch acceptance.
"""
import argparse
from datetime import datetime
from pathlib import Path
import secrets
import sys
import time

import meoave

CONFIG = meoave.load_config()
PROJECT = meoave.project_dir(CONFIG)
sys.path.insert(0, str(PROJECT))
import flash_usb as usb


def fields(line, prefix):
    if not line.startswith(prefix + " "):
        raise ValueError("Unexpected reply: " + line)
    reason = None
    if " reason=" in line:
        line, reason = line.split(" reason=", 1)
    result = {}
    for item in line[len(prefix) + 1:].split():
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        if key in result:
            raise ValueError("Duplicate reply field: " + key)
        result[key] = value
    if reason is not None:
        result["reason"] = reason
    return result


def validate_health(reply, volume=None):
    expected = {"chip": "8389", "addr": "0x12", "init_error": "0",
                "errors": "0", "clk": "00", "adc": "60", "dac": "60"}
    if any(reply.get(k) != v for k, v in expected.items()):
        raise RuntimeError("Codec identity/configuration/read failed: " + str(reply))
    if not 1 <= int(reply["attempts"]) <= 3:
        raise RuntimeError("Invalid startup attempt count")
    mux = [int(v, 16) for v in reply["mux"].split("/")]
    if len(mux) != 2 or [v >> 4 for v in mux] != [6, 5]:
        raise RuntimeError("Verified single-ended microphone route changed")
    if volume is not None:
        reg = 0 if volume == 0 else 0xBF - (96 * (100 - volume) ** 2) // 10000
        if reply["vol"].upper() != f"{reg:02X}/{reg:02X}":
            raise RuntimeError("DAC readback does not match UI volume")


class Link:
    def __init__(self, stream, report):
        self.stream, self.report = stream, report
        self.pending = b""

    def send(self, command):
        if not command or "\n" in command or "\r" in command or not command.isascii():
            raise ValueError("One ASCII command required")
        self.report["transcript"].append({"tx": command})
        self.stream.write((command + "\n").encode("ascii"))
        self.stream.flush()  # Never retry a mutating command on a lost reply.

    def wait(self, prefix, timeout=3, token=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" not in self.pending:
                self.pending += self.stream.read(1024)
                if len(self.pending) > 16384:
                    raise RuntimeError("Unterminated/oversized USB reply")
                continue
            raw, self.pending = self.pending.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            self.report["transcript"].append({"rx": line})
            if "Guru Meditation" in line or "panic'ed" in line:
                raise RuntimeError("P4 panic: " + line)
            if line.startswith(prefix + " "):
                reply = fields(line, prefix)
                if token is None or reply.get("token") == token:
                    return reply
        raise TimeoutError("No matching reply for " + prefix)

    def request(self, command, prefix):
        self.send(command)
        return self.wait(prefix)

    def health(self, volume=None):
        token = secrets.token_hex(6)
        self.send("AUDIO HEALTH " + token)
        reply = self.wait("MEO:AUDIO_HEALTH", token=token)
        validate_health(reply, volume)
        return reply

    def audio(self):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            self.send("UI")  # Read-only query may be polled on a dropped record.
            try:
                return self.wait("MEO:UI_AUDIO", timeout=0.6)
            except TimeoutError:
                pass
        raise TimeoutError("No UI audio status after bounded read-only polling")

    def volume(self, percent):
        ack = self.request(f"UI VOLUME {percent}", "MEO:UI_VOLUME")
        if ack.get("queued") != "1":
            raise RuntimeError("UI volume request refused")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.audio().get("volume") == str(percent):
                time.sleep(0.7)  # NVS debounce, not an extra reset/recovery.
                return self.health(percent)
            time.sleep(0.05)
        raise TimeoutError("UI volume did not settle")

    def page(self, name):
        self.send("UI GO " + name)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            self.send("UI")
            try:
                reply = self.wait("MEO:UI", timeout=0.6)
                # Consume the second line of this same status request so it
                # cannot masquerade as a later UI_AUDIO query's fresh reply.
                self.wait("MEO:UI_AUDIO", timeout=0.4)
                if reply.get("page") == name:
                    return
            except TimeoutError:
                pass  # Do not replay navigation; only query its outcome.
            time.sleep(0.05)
        raise TimeoutError("UI page did not settle: " + name)

    def microphone(self):
        token = secrets.token_hex(6)
        self.send("MIC STATUS " + token)
        return self.wait("MEO:MIC", token=token)

    def preview(self, double=False):
        before = int(self.audio()["preview_seq"])
        self.send("UI PREVIEW")
        if double:
            self.send("UI PREVIEW")
        if self.wait("MEO:PREVIEW").get("queued") != "1":
            raise RuntimeError("Preview was refused")
        if double and self.wait("MEO:PREVIEW").get("queued") != "0":
            raise RuntimeError("Duplicate preview was not blocked")
        saw_busy = False
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            state = self.audio()
            sequence = int(state["preview_seq"])
            # UI_AUDIO reports the last view delivered to LVGL at 5 Hz, not
            # the worker's immediate state. An accepted request may therefore
            # precede that view; poll read-only status, never replay the tap.
            if sequence == before and not saw_busy:
                time.sleep(0.05)
                continue
            if sequence != before + 1:
                raise RuntimeError("Preview generation mismatch")
            saw_busy |= state["preview_busy"] == "1"
            if state["preview_busy"] == "0":
                if not saw_busy or state["preview_error"] != "0":
                    raise RuntimeError("Preview failed or busy lifecycle not observed: " + str(state))
                token = secrets.token_hex(6)
                self.send("AUDIO STATUS " + token)
                status = self.wait("MEO:AUDIO_LOOP", token=token)
                if status.get("pa") != "0" or status.get("busy") != "0":
                    raise RuntimeError("Audio output did not return idle")
                return state
            time.sleep(0.05)
        raise TimeoutError("Preview did not finish")

    def loopback(self):
        token = secrets.token_hex(6)
        self.send("AUDIO LOOPBACK " + token + " 400")
        phases = {}
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            reply = self.wait("MEO:AUDIO_LOOP", timeout=max(0.1, deadline-time.monotonic()), token=token)
            if "phase" in reply:
                if reply["phase"] in phases:
                    raise RuntimeError("Duplicate acoustic-test phase")
                phases[reply["phase"]] = reply
            if "verdict" in reply:
                evidence = {"phases": phases, "result": reply}
                self.report["loopback"] = evidence
                if reply["verdict"] != "PASS" or set(phases) != {
                        "baseline", "tone", "dac_mute", "pa_off", "restore"}:
                    raise RuntimeError("Incomplete/failed acoustic test: " + str(reply))
                return evidence
        raise TimeoutError("No acoustic-test verdict")


def run(build, report):
    expected = usb.image_identity(build)
    report["image"] = expected
    before = usb.check_application(expected["elf_sha"], timeout=10)
    report["before"] = before
    with usb.open_native(before["port"], usb.APP_PIDS) as stream:
        link = Link(stream, report)
        report["startup_health"] = link.health()
        report["volume_30"] = link.volume(30)
        link.page("speaker")
        report["preview_double"] = link.preview(double=True)
        report["volume_0"] = link.volume(0)
        report["preview_muted"] = link.preview()
        link.volume(30)
        report["preview_repeat"] = link.preview()
        report["microphone_pages"] = []
        for _ in range(3):
            link.page("mic")
            time.sleep(0.3)
            active = link.microphone()
            link.page("speaker")
            time.sleep(0.1)
            stopped = link.microphone()
            report["microphone_pages"].append({"active": active, "stopped": stopped})
            if active.get("running") != "1" or stopped.get("running") != "0":
                raise RuntimeError("Microphone page start/stop mismatch")
        report["controls_ok"] = True
        report["loopback"] = link.loopback()
        report["final_health"] = link.health(30)
        link.page("eyes")
    after = usb.check_application(expected["elf_sha"], timeout=5)
    report["after"] = after
    if after["boot_id"] != before["boot_id"]:
        raise RuntimeError("Unexpected P4 reboot during audio acceptance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=PROJECT / meoave.cfg(CONFIG, "build", "product_dir", "build_meoave"))
    parser.add_argument("--report", type=Path, required=True, help="new JSON report path (existing files refused)")
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Report already exists; choose a new path to preserve evidence")
    report = {"ok": False, "started": datetime.now().astimezone().isoformat(), "transcript": [],
              "scope": "Native USB product audio only; no C5 command, flash, reset, calibration or manual pin operation",
              "visual": "NOT_RETESTED", "subjective_audio": "NOT_RETESTED"}
    try:
        run(args.build_dir, report)
        report["ok"] = True
        print("PASS: verified codec; UI melody 30%/0%; duplicate rejection; mic page lifecycle; acoustic loopback; unchanged P4 boot")
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print("FAIL: " + report["error"], file=sys.stderr)
        return 1
    finally:
        report["finished"] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == "__main__":
    sys.exit(main())
