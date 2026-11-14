"""Native product-audio parser/guards; no serial hardware is accessed."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import audio_acceptance as a


class Stream:
    def __init__(self, data=b"", fragment=7):
        self.data, self.fragment = data, fragment
        self.writes = []

    def read(self, count):
        count = min(count, self.fragment)
        result, self.data = self.data[:count], self.data[count:]
        return result

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        pass


class AcceptanceTests(unittest.TestCase):
    def health(self, **changes):
        result = dict(chip="8389", addr="0x12", init_error="0", errors="0",
                      clk="00", adc="60", dac="60", attempts="1", mux="6E/5E", vol="90/90")
        result.update(changes)
        return result

    def test_good_health_preserves_single_ended_route(self):
        a.validate_health(self.health(), 30)
        a.validate_health(self.health(vol="00/00", attempts="3", mux="64/54"), 0)

    def test_phantom_identity_or_partial_initialization_rejected(self):
        for changes in (dict(chip="FFFF"), dict(addr="0x10"), dict(init_error="264"),
                        dict(errors="1"), dict(dac="FF"), dict(adc="63"),
                        dict(clk="40"), dict(attempts="4"), dict(attempts="0")):
            with self.subTest(changes=changes), self.assertRaises(RuntimeError):
                a.validate_health(self.health(**changes))

    def test_changed_route_and_wrong_or_partial_volume_rejected(self):
        for changes in (dict(mux="1E/1E"), dict(mux="5E/6E"),
                        dict(vol="BF/BF"), dict(vol="90/00")):
            with self.subTest(changes=changes), self.assertRaises(RuntimeError):
                a.validate_health(self.health(**changes), 30)

    def test_duplicate_fields_rejected(self):
        with self.assertRaises(ValueError):
            a.fields("MEO:PREVIEW queued=1 queued=0", "MEO:PREVIEW")

    def test_reason_kept_as_complete_diagnostic_not_extra_fields(self):
        reply = a.fields("MEO:AUDIO_LOOP verdict=FAIL reason=no acoustic tone (tone=419 base=133)", "MEO:AUDIO_LOOP")
        self.assertEqual(reply, {"verdict": "FAIL", "reason": "no acoustic tone (tone=419 base=133)"})

    def test_fragmented_reply_and_stale_token(self):
        stream = Stream(b"noise\nMEO:MIC token=old running=0\nMEO:MIC token=fresh running=1\n")
        link = a.Link(stream, {"transcript": []})
        self.assertEqual(link.wait("MEO:MIC", token="fresh")["running"], "1")

    def test_command_injection_never_sent(self):
        stream = Stream()
        link = a.Link(stream, {"transcript": []})
        for command in ("UI\nSYS RESTART x", "UI\rUI PREVIEW", "", "你好"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                link.send(command)
        self.assertEqual(stream.writes, [])

    def test_mutating_command_not_resent_on_missing_reply(self):
        stream = Stream()
        link = a.Link(stream, {"transcript": []})
        link.send("UI PREVIEW")
        with self.assertRaises(TimeoutError):
            link.wait("MEO:PREVIEW", timeout=0.01)
        self.assertEqual(stream.writes, [b"UI PREVIEW\n"])

    def test_panic_never_passes(self):
        link = a.Link(Stream(b"Guru Meditation Error\nMEO:PREVIEW queued=1\n"), {"transcript": []})
        with self.assertRaisesRegex(RuntimeError, "panic"):
            link.wait("MEO:PREVIEW")

    def test_preview_waits_for_gui_generation_without_replaying(self):
        def state(seq, busy):
            return dict(preview_seq=str(seq), preview_busy=str(busy), preview_error="0")
        stream = Stream(b"MEO:PREVIEW queued=1\nMEO:AUDIO_LOOP token=abc busy=0 pa=0\n")
        link = a.Link(stream, {"transcript": []})
        with patch.object(a.secrets, "token_hex", return_value="abc"), \
                patch.object(a.time, "sleep"), \
                patch.object(link, "audio", side_effect=[state(2, 0), state(2, 0), state(3, 1), state(3, 0)]):
            self.assertEqual(link.preview()["preview_seq"], "3")
        self.assertEqual(stream.writes.count(b"UI PREVIEW\n"), 1)

    def test_preview_rejects_unexpected_generation_or_unobserved_busy(self):
        for seq, busy in ((4, 1), (3, 0)):
            link = a.Link(Stream(b"MEO:PREVIEW queued=1\n"), {"transcript": []})
            states = [dict(preview_seq="2", preview_busy="0", preview_error="0"),
                      dict(preview_seq=str(seq), preview_busy=str(busy), preview_error="0")]
            with self.subTest(seq=seq, busy=busy), patch.object(link, "audio", side_effect=states):
                with self.assertRaises(RuntimeError):
                    link.preview()

    def test_lost_page_record_only_repeats_read_only_query(self):
        stream = Stream()
        link = a.Link(stream, {"transcript": []})
        with patch.object(a.time, "sleep"), patch.object(link, "wait", side_effect=[
                TimeoutError("dropped page status"), {"page": "mic"}, {"preview_seq": "0"}]):
            link.page("mic")
        self.assertEqual(stream.writes, [b"UI GO mic\n", b"UI\n", b"UI\n"])

    def test_lost_audio_record_only_repeats_read_only_query(self):
        stream = Stream()
        link = a.Link(stream, {"transcript": []})
        with patch.object(link, "wait", side_effect=[TimeoutError("dropped audio status"), {"preview_seq": "0"}]):
            self.assertEqual(link.audio()["preview_seq"], "0")
        self.assertEqual(stream.writes, [b"UI\n", b"UI\n"])

    def test_full_acoustic_evidence_required(self):
        def line(body):
            return ("MEO:AUDIO_LOOP token=abc " + body + "\n").encode()
        full = b"".join(line("phase=" + p + " l=1 r=2") for p in
                        ("baseline", "tone", "dac_mute", "pa_off", "restore"))
        for data, expected in ((full + line("verdict=PASS score=100"), True),
                               (line("verdict=PASS score=100"), False),
                               (full + line("verdict=FAIL score=0"), False),
                               (full + line("phase=tone l=1 r=2"), False)):
            link = a.Link(Stream(data), {"transcript": []})
            with self.subTest(expected=expected), patch.object(a.secrets, "token_hex", return_value="abc"):
                if expected:
                    self.assertEqual(link.loopback()["result"]["score"], "100")
                else:
                    with self.assertRaises(RuntimeError):
                        link.loopback()


if __name__ == "__main__":
    unittest.main()
