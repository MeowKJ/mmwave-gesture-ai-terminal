#!/usr/bin/env python3
"""Bounded Edge-TTS latency probe, not an Azure Speech or device benchmark.

Optional isolated dependencies: edge-tts==7.3.1, mutagen==1.47.0.
Only the synthetic sentences below are sent. No microphone, firmware, keys,
account creation, persistent service, application retry or device access.
"""
from __future__ import annotations

import argparse
import asyncio
from datetime import datetime, timedelta, timezone
import hashlib
from importlib.metadata import version
import io
import json
from pathlib import Path
import platform
import statistics
import time
from urllib.request import getproxies

CASES = (
    ("short", "你好，我在这里。"),
    ("medium", "今天过得怎么样？如果有点累，我们就一起慢慢呼吸。"),
    ("long", "不用急着把所有事情都做好。你可以先喝一点水，伸个懒腰，再告诉我今天发生了什么。我会认真听，也会一直在这里陪着你。"),
)


async def collect_audio(events, clock=time.perf_counter):
    """Measure first/last AUDIO, not websocket connection or text metadata."""
    started = clock()
    first = last = None
    blocks = []
    async for event in events:
        if event.get("type") != "audio" or not event.get("data"):
            continue
        now = clock()
        if first is None:
            first = now
        last = now
        blocks.append(event["data"])
    finished = clock()
    if first is None:
        raise ValueError("Stream ended without audio")
    return b"".join(blocks), {
        "first_audio_ms": round((first-started)*1000, 3),
        "last_audio_ms": round((last-started)*1000, 3),
        "stream_done_ms": round((finished-started)*1000, 3),
        "audio_chunks": len(blocks),
    }


def summarize(rows):
    result = {}
    for case, _ in CASES:
        good = [r for r in rows if r["case"] == case and r["status"] == "ok"]
        item = {"successes": len(good),
                "failures": sum(r["case"] == case and r["status"] != "ok" for r in rows)}
        for field in ("first_audio_ms", "last_audio_ms", "stream_done_ms", "audio_duration_ms"):
            values = [r[field] for r in good]
            if values:
                item[field] = {"min": min(values), "median": statistics.median(values), "max": max(values)}
        result[case] = item
    return result


async def benchmark(args, report):
    import edge_tts
    from mutagen.mp3 import MP3

    failures = 0
    # Round-robin lengths avoid measuring every short request before every long
    # request. No claim of HTTP connection reuse or provider-side cold starts.
    for repetition in range(1, args.repeats+1):
        for case, text in CASES:
            row = {"case": case, "repetition": repetition, "text": text,
                   "characters": len(text), "started_at": timestamp()}
            try:
                request = edge_tts.Communicate(text, args.voice, rate="+0%", volume="+0%")
                data, timing = await asyncio.wait_for(collect_audio(request.stream()), args.timeout)
                # Decoding/inspection and disk I/O happen AFTER network timing.
                info = MP3(io.BytesIO(data)).info
                duration = info.length*1000
                if duration <= 0:
                    raise ValueError("Invalid MP3 duration")
                path = args.out/f"{case}-{repetition:02}.mp3"
                path.write_bytes(data)
                row.update(timing, status="ok", audio_bytes=len(data),
                           audio_sha256=hashlib.sha256(data).hexdigest(),
                           audio_duration_ms=round(duration, 3), sample_rate=info.sample_rate,
                           channels=info.channels, audio_file=str(path.resolve()),
                           receive_time_over_audio_duration=round(timing["stream_done_ms"]/duration, 4))
                failures = 0
                print(f"{case} #{repetition}: first={timing['first_audio_ms']:.0f}ms "
                      f"last={timing['last_audio_ms']:.0f}ms done={timing['stream_done_ms']:.0f}ms "
                      f"audio={duration:.0f}ms", flush=True)
            except Exception as exc:
                failures += 1
                # Do not log request URLs, headers, proxy addresses or tokens.
                row.update(status="error", error_type=type(exc).__name__,
                           http_status=getattr(exc, "status", None))
                print(f"{case} #{repetition}: FAILED {type(exc).__name__}", flush=True)
            report["requests"].append(row)
            report["summary"] = summarize(report["requests"])
            save(args.out, report)
            if failures >= 2:
                report["stop_reason"] = "Two consecutive failures; no hidden recovery/retry"
                return
            await asyncio.sleep(0.5)


def timestamp():
    # Current task's Shanghai time; do not require a separately installed IANA
    # tzdata package on Windows just to date a latency report.
    return datetime.now(timezone(timedelta(hours=8), "Asia/Shanghai")).isoformat()


def save(out, report):
    (out/"report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="New output directory")
    parser.add_argument("--voice", default="zh-CN-XiaoxiaoNeural")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=20)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 5 or not 1 <= args.timeout <= 60:
        parser.error("repeats must be 1..5 and timeout 1..60 seconds")
    if args.out.exists():
        parser.error("--out must be a NEW directory; previous evidence is preserved")
    # Check dependencies before creating a partial output directory.
    packages = {name: version(name) for name in ("edge-tts", "aiohttp", "mutagen")}
    report = {
        "provider": "Microsoft Edge online read-aloud via third-party edge-tts; NOT Azure Speech API",
        "scope": "PC network TTS only; excludes recording, ASR, LLM, P4 transfer, playback/decode latency and A/V sync",
        "started_at": timestamp(), "python": platform.python_version(), "packages": packages,
        "voice": args.voice, "rate": "+0%", "volume": "+0%", "repeats": args.repeats,
        "request_timeout_seconds": args.timeout, "proxy_types_present": sorted(getproxies()),
        "request_policy": "Sequential new Communicate objects; no application retry; SDK-internal behavior is included in timings",
        "timing": "perf_counter before streaming to first/last nonempty audio event and stream exhaustion; excludes file writes",
        "caveats": ["First audio bytes are not proof of audible playback",
                    "Three samples per length do not establish P95 or long-term reliability",
                    "No claim of service-side cold/warm state or a reused network connection"],
        "requests": [], "completed": False,
    }
    args.out.mkdir(parents=True)
    try:
        asyncio.run(benchmark(args, report))
        report["completed"] = len(report["requests"]) == len(CASES)*args.repeats
    finally:
        report["finished_at"] = timestamp()
        save(args.out, report)
    return 0 if report["completed"] and all(r["status"] == "ok" for r in report["requests"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
