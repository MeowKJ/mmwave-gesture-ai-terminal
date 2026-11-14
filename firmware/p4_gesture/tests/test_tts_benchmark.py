"""Offline checks for the latency probe; does not contact any TTS service."""
import asyncio
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "tts_benchmark", Path(__file__).resolve().parents[3]/"tools/benchmark_tts.py")
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


async def events(items):
    for item in items:
        yield item


class LatencyProbeTests(unittest.TestCase):
    def test_timestamp_has_requested_client_offset_without_tzdata(self):
        self.assertTrue(BENCH.timestamp().endswith("+08:00"))

    def test_metadata_and_empty_bytes_are_not_first_audio(self):
        clock = iter([10.0, 10.2, 10.5, 10.6])
        data, result = asyncio.run(BENCH.collect_audio(events([
            {"type": "WordBoundary", "text": "hello"},
            {"type": "audio", "data": b""},
            {"type": "audio", "data": b"a"},
            {"type": "audio", "data": b"bc"},
        ]), clock=lambda: next(clock)))
        self.assertEqual(data, b"abc")
        self.assertEqual(result, dict(first_audio_ms=200, last_audio_ms=500,
                                      stream_done_ms=600, audio_chunks=2))

    def test_no_audio_is_failure_not_zero_latency(self):
        with self.assertRaisesRegex(ValueError, "without audio"):
            asyncio.run(BENCH.collect_audio(events([{"type": "WordBoundary"}])))

    def test_failures_do_not_become_fast_samples(self):
        rows = [{"case": "short", "status": "error"}]
        for value in (300, 100, 200):
            rows.append(dict(case="short", status="ok", first_audio_ms=value,
                             last_audio_ms=value+20, stream_done_ms=value+30,
                             audio_duration_ms=1000))
        summary = BENCH.summarize(rows)["short"]
        self.assertEqual(summary["successes"], 3)
        self.assertEqual(summary["failures"], 1)
        self.assertEqual(summary["first_audio_ms"], dict(min=100, median=200, max=300))
        self.assertNotIn("first_audio_ms", BENCH.summarize(rows)["long"])


if __name__ == "__main__":
    unittest.main()
