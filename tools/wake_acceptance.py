#!/usr/bin/env python3
"""ESP-SR native USB monitor. No text injection, playback, reset, or C5 access.

Machine status verifies the input/inference pipeline, NOT recognition quality.
Acoustic detections are counted separately and require a real external voice.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import time
from audio_acceptance import Link, fields, usb, CONFIG, PROJECT, meoave


def run(build, seconds, minimum, report):
    identity = usb.image_identity(build)
    report["image"] = identity
    before = usb.check_application(identity["elf_sha"], 10)
    report["before"] = before
    with usb.open_native(before["port"], usb.APP_PIDS) as stream:
        link = Link(stream, report)
        link.page("eyes")
        first = link.request("UI", "MEO:WAKE_STATUS")
        report["initial"] = first
        if any(first.get(key) != "1" for key in ("linked", "loaded", "keyword_ready")) or first.get("guarded") == "1":
            raise RuntimeError("ESP-SR model is not ready: " + str(first))
        baseline, samples = int(first["acoustic"]), []
        report["samples"] = samples  # Preserve partial evidence on disconnect.
        deadline = time.monotonic() + seconds
        rebuilding_since = None
        while time.monotonic() < deadline:
            state = link.request("UI", "MEO:WAKE_STATUS")
            samples.append(state)
            if (state.get("error") == "MODEL_REBUILDING" and state.get("linked") == "1"
                    and state.get("guarded") != "1"):
                now = time.monotonic()
                if rebuilding_since is None:
                    rebuilding_since = now
                if now - rebuilding_since > 3:
                    raise RuntimeError("Model rebuild exceeded 3 seconds: " + str(state))
                time.sleep(0.2)
                continue
            rebuilding_since = None
            if state.get("error") != "NONE":
                raise RuntimeError("ESP-SR runtime error: " + str(state))
            if any(state.get(key) != "1" for key in ("linked", "loaded", "keyword_ready")) or state.get("guarded") == "1":
                raise RuntimeError("ESP-SR readiness lost during monitor: " + str(state))
            time.sleep(0.8)
        if not samples or int(samples[-1]["chunks"]) <= int(first["chunks"]):
            raise RuntimeError("No real microphone inference progress")
        final = samples[-1]
        if any(final.get(key) != "1" for key in ("linked", "loaded", "keyword_ready")) or final.get("error") != "NONE":
            raise RuntimeError("Model not ready at end of monitor: " + str(final))
        if final.get("armed") != "1" or final.get("requested") != "1":
            raise RuntimeError("Wake is not armed on the AI page: " + str(final))
        detections = int(final["acoustic"]) - baseline
        report["new_acoustic_detections"] = detections
        report["events"] = [fields(item["rx"], "MEO:WAKE") for item in report["transcript"]
                            if item.get("rx", "").startswith("MEO:WAKE ") and "source=ACOUSTIC_ESP_SR" in item["rx"]]
        report["acoustic_acceptance"] = "DETECTED_NOT_RATE_QUALIFIED" if detections else "PENDING_EXTERNAL_VOICE"
        report["final"] = final
        if int(final["text_tests"]) != int(first["text_tests"]):
            raise RuntimeError("Text injection occurred during acoustic monitor")
    after = usb.check_application(identity["elf_sha"], 5)
    report["after"] = after
    if before["boot_id"] != after["boot_id"]:
        raise RuntimeError("Unexpected reset during wake monitor")
    report["fault"] = usb.read_fault_diagnostic(after["port"])
    if report["fault"].get("valid") != "0":
        raise RuntimeError("P4 fault present or unavailable")
    if detections < minimum:
        raise RuntimeError(f"Only {detections} new acoustic detections; need {minimum}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=PROJECT / meoave.cfg(CONFIG, "build", "product_dir", "build_meoave"))
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--listen-seconds", type=int, default=30)
    parser.add_argument("--min-detections", type=int, default=0)
    args = parser.parse_args()
    if not 3 <= args.listen_seconds <= 1800 or not 0 <= args.min_detections <= 50:
        parser.error("listen-seconds must be 3..1800 and min-detections 0..50")
    if args.report.exists():
        parser.error("Choose a fresh report path; preserve prior evidence")
    report = {"ok": False, "started": datetime.now().astimezone().isoformat(), "transcript": [],
              "scope": "P4 microphone + real ESP-SR status/events; NO text injection/C5/reset/playback",
              "visual": "PENDING_USER", "acoustic_acceptance": "PENDING_EXTERNAL_VOICE"}
    try:
        run(args.build_dir, args.listen_seconds, args.min_detections, report)
        report["ok"] = True
        print(f"PASS: real input/inference pipeline; acoustic detections={report['new_acoustic_detections']}; "
              + report["acoustic_acceptance"] + "; visual pending user")
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        try:
            port = usb.app_port()
            if port:
                report["fault_after_failure"] = usb.read_fault_diagnostic(port.device)
        except Exception as diagnostic:
            report["diagnostic_error"] = str(diagnostic)
        print("FAIL: " + report["error"])
        return 1
    finally:
        report["finished"] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == "__main__":
    raise SystemExit(main())
