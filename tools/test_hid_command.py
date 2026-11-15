#!/usr/bin/env python3
"""Offline tests for the Meoave desktop HID protocol implementation."""
from __future__ import annotations

import unittest

from hid_command import (
    COMMANDS,
    HEADER_SIZE,
    KIND_COMMAND,
    KIND_EVENT,
    KIND_LOG,
    KIND_REPLY,
    PAYLOAD_SIZE,
    REPORT_ID,
    REPORT_SIZE,
    MessageAssembler,
    decode_frame,
    fragment_message,
    normalize_report,
)


class HidProtocolTests(unittest.TestCase):
    def test_registry_keeps_production_controls_not_demo_commands(self) -> None:
        commands = {spec.command for spec in COMMANDS}
        self.assertTrue({"AI STATUS", "AI CANCEL"}.issubset(commands))
        self.assertFalse(any("DEMO" in command for command in commands))

    def test_report_fits_hid_endpoint_with_report_id(self) -> None:
        frame = fragment_message("PING", KIND_COMMAND, 0)[0]
        self.assertEqual(REPORT_SIZE, 63)
        self.assertEqual(len(frame), REPORT_SIZE)
        self.assertEqual(len(bytes([REPORT_ID]) + frame), 64)

    def test_short_and_long_fragmentation(self) -> None:
        for text in ("PING", "x" * PAYLOAD_SIZE, "x" * (PAYLOAD_SIZE + 1), "你好" * 80):
            frames = fragment_message(text, KIND_COMMAND, 7)
            self.assertLessEqual(len(frames), 9)
            assembler = MessageAssembler()
            result = None
            for frame in frames:
                result = assembler.feed(frame)
            self.assertIsNotNone(result)
            self.assertEqual(result.text, text)
            self.assertEqual(result.msg_id, 7)
            self.assertEqual(assembler.dropped, 0)

    def test_report_id_compatibility(self) -> None:
        frame = fragment_message("PING", KIND_REPLY, 1)[0]
        self.assertEqual(normalize_report(frame), frame)
        self.assertEqual(normalize_report(bytes([REPORT_ID]) + frame), frame)
        kind, msg_id, index, total, body = decode_frame(bytes([REPORT_ID]) + frame)
        self.assertEqual((kind, msg_id, index, total), (KIND_REPLY, 1, 0, 1))
        self.assertEqual(body[:4], b"PING")

    def test_interleaved_streams(self) -> None:
        reply = fragment_message("reply-" + "r" * 100, KIND_REPLY, 1)
        log = fragment_message("log-" + "l" * 100, KIND_LOG, 2)
        event = fragment_message("event", KIND_EVENT, 3)
        assembler = MessageAssembler()
        result = []
        for index in range(max(len(reply), len(log))):
            if index < len(reply):
                item = assembler.feed(reply[index])
                if item:
                    result.append(item)
            if index < len(log):
                item = assembler.feed(log[index])
                if item:
                    result.append(item)
        item = assembler.feed(event[0])
        if item:
            result.append(item)
        self.assertEqual({item.text for item in result}, {
            "reply-" + "r" * 100, "log-" + "l" * 100, "event"})
        self.assertEqual(assembler.dropped, 0)

    def test_sequence_error_is_dropped(self) -> None:
        frames = fragment_message("a" * (PAYLOAD_SIZE + 3), KIND_REPLY, 4)
        assembler = MessageAssembler()
        self.assertIsNone(assembler.feed(frames[1]))
        self.assertEqual(assembler.dropped, 1)
        self.assertIsNone(assembler.feed(frames[0]))
        self.assertIsNotNone(assembler.feed(frames[1]))

    def test_nul_padding_is_only_removed_at_end(self) -> None:
        text = "A\x00B"
        frame = fragment_message(text, KIND_REPLY, 8)[0]
        assembler = MessageAssembler()
        result = assembler.feed(frame)
        self.assertIsNotNone(result)
        self.assertEqual(result.text, text)


if __name__ == "__main__":
    unittest.main()
