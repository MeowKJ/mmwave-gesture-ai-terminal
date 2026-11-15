#!/usr/bin/env python3
"""Desktop HID command client and acceptance runner for Meoave.

The product exposes one vendor-defined HID collection for the bidirectional
command link.  The link is deliberately independent from CDC so a desktop
client can operate without opening COM3/COM10.  This module is also usable as
an importable protocol library; the offline tests exercise it without a HID
device or the optional ``hidapi`` package.

Examples::

    python tools/hid_command.py --list
    python tools/hid_command.py --command "LINK CAPS c1"
    python tools/hid_command.py --suite safe --report test-results/hid-safe.json
    python tools/hid_command.py --suite full --allow-side-effects

``full`` never performs destructive operations such as reboot, download,
credential writes, or C5 programming.  Those commands remain available via
``--command`` and are explicitly labelled in ``COMMANDS`` below.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time
from typing import Any, Iterable, Optional

try:  # hidapi is intentionally optional for offline protocol tests.
    import hid as _hid
except ImportError:  # pragma: no cover - exercised by machines without hidapi
    _hid = None


VID = 0x303A
PID = 0x4005
REPORT_ID = 4
# HIDAPI includes one Report ID byte in Windows writes/reads.  Keep the
# actual report at 63 bytes so the on-wire packet is 64 bytes, the maximum
# interrupt packet size used by both the FS and HS descriptors.
REPORT_SIZE = 63
HEADER_SIZE = 4
PAYLOAD_SIZE = REPORT_SIZE - HEADER_SIZE
MAX_MESSAGE = 512
MAX_FRAGMENTS = (MAX_MESSAGE + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE

KIND_REPLY = ord("R")
KIND_LOG = ord("L")
KIND_EVENT = ord("E")
KIND_COMMAND = ord("C")


class HidUnavailable(RuntimeError):
    """The optional hidapi module is not installed."""


class HidProtocolError(RuntimeError):
    """A malformed HID report or an impossible fragment sequence."""


@dataclass(frozen=True)
class CommandSpec:
    name: str
    command: str
    expected: str = ""
    category: str = "safe"
    side_effect: bool = False
    destructive: bool = False
    timeout_ms: int = 1800
    collect_ms: int = 0


@dataclass
class HidMessage:
    kind: int
    msg_id: int
    text: str
    fragments: int

    @property
    def kind_name(self) -> str:
        return chr(self.kind) if 32 <= self.kind <= 126 else f"0x{self.kind:02x}"


@dataclass
class CommandResult:
    command: str
    ok: bool
    elapsed_ms: int
    replies: list[str] = field(default_factory=list)
    logs: list[str] = field(default_factory=list)
    events: list[str] = field(default_factory=list)
    error: str = ""

    @property
    def first_reply(self) -> str:
        return self.replies[0] if self.replies else ""


# The registry is intentionally explicit.  It is the desktop-side source of
# truth for acceptance coverage; the generic --command path still permits new
# firmware commands before this table is updated.
COMMANDS: tuple[CommandSpec, ...] = (
    CommandSpec("link_caps", "LINK CAPS {token}", "caps=1"),
    CommandSpec("link_ping", "LINK PING {token}", "MEO:PONG"),
    CommandSpec("link_status", "LINK STATUS {token}", "MEO:LINK"),
    CommandSpec("sys_info", "SYS INFO {token}", "SYS:INFO"),
    CommandSpec("sys_power", "SYS POWER {token}", "SYS:POWER"),
    CommandSpec("sys_fault", "SYS FAULT {token}", "SYS:FAULT"),
    CommandSpec("ping", "PING", "FLS2:PONG"),
    CommandSpec("status", "STATUS", "FLS2:STATUS", collect_ms=250),
    CommandSpec("ui", "UI", "MEO:UI", collect_ms=250),
    CommandSpec("ai_status", "AI STATUS", "MEO:AI", collect_ms=250),
    CommandSpec("audio_health", "AUDIO HEALTH {token}", "MEO:AUDIO_HEALTH"),
    CommandSpec("audio_status", "AUDIO STATUS {token}", "MEO:AUDIO_LOOP"),
    CommandSpec("spk_volume_query", "SPK VOL", "MEO:SPK"),
    CommandSpec("mic_status", "MIC STATUS {token}", "MEO:MIC"),
    CommandSpec("wifi_status", "WIFI STATUS {token}", "MEO:WIFI"),
    CommandSpec("wifi_aps", "WIFI APS {token}", "MEO:WIFI_SCAN"),
    CommandSpec("ui_home", "UI GO eyes", "", category="functional", side_effect=True),
    CommandSpec("ui_apps", "UI GO apps", "", category="functional", side_effect=True),
    CommandSpec("ui_settings", "UI GO settings", "", category="functional", side_effect=True),
    CommandSpec("ui_volume", "UI VOLUME 70", "MEO:UI_VOLUME", category="functional", side_effect=True),
    CommandSpec("ui_cue", "UI CUE", "MEO:CUE", category="functional", side_effect=True),
    CommandSpec("ui_preview", "UI PREVIEW", "MEO:PREVIEW", category="functional", side_effect=True),
    CommandSpec("wifi_scan", "WIFI SCAN {token}", "MEO:WIFI_ACCEPT", category="functional", side_effect=True, timeout_ms=2500),
    CommandSpec("spk_test", "SPK TONE 250 20", "MEO:SPK", category="functional", side_effect=True),
    CommandSpec("mic_start", "MIC START {token}", "MEO:MIC", category="functional", side_effect=True),
    CommandSpec("mic_stop", "MIC STOP {token}", "MEO:MIC", category="functional", side_effect=True),
    # These are documented but never part of an unattended suite.
    CommandSpec("ai_cancel", "AI CANCEL", "MEO:AI_CANCEL", category="manual", side_effect=True),
    CommandSpec("sys_restart", "SYS RESTART {token}", "SYS:RESTART", category="manual", side_effect=True, destructive=True),
    CommandSpec("download", "DOWNLOAD", "", category="manual", side_effect=True, destructive=True),
)


def _token(prefix: str = "h") -> str:
    # Monotonic time is enough for a single host process and keeps tokens
    # within the firmware's alphanumeric validation rule.
    return f"{prefix}{int(time.monotonic_ns() // 1_000_000) & 0xFFFFFFF:x}"


def fragment_message(text: str, kind: int = KIND_COMMAND, msg_id: int = 0) -> list[bytes]:
    """Encode one command/event into fixed-size protocol reports."""
    raw = text.encode("utf-8")
    if len(raw) > MAX_MESSAGE:
        raise ValueError(f"message is {len(raw)} bytes; maximum is {MAX_MESSAGE}")
    total = max(1, (len(raw) + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE)
    frames: list[bytes] = []
    for index in range(total):
        frame = bytearray(REPORT_SIZE)
        frame[0:4] = bytes((kind & 0xFF, msg_id & 0xFF, index, total))
        chunk = raw[index * PAYLOAD_SIZE:(index + 1) * PAYLOAD_SIZE]
        frame[HEADER_SIZE:HEADER_SIZE + len(chunk)] = chunk
        frames.append(bytes(frame))
    return frames


def normalize_report(data: Iterable[int] | bytes | bytearray) -> bytes:
    """Return a 63-byte payload from hidapi's 63/64-byte read variants.

    Windows hidapi normally includes the report ID (64 bytes).  Some backends
    strip it (63 bytes), and both variants exist in developer machines.  A
    report ID is not part of the meo_link frame itself.
    """
    raw = bytes(data)
    if len(raw) >= REPORT_SIZE + 1 and raw[0] == REPORT_ID:
        payload = raw[1:1 + REPORT_SIZE]
    elif len(raw) >= REPORT_SIZE:
        payload = raw[:REPORT_SIZE]
    elif len(raw) >= HEADER_SIZE:
        payload = raw + bytes(REPORT_SIZE - len(raw))
    else:
        raise HidProtocolError(f"short HID report: {len(raw)} bytes")
    if len(payload) != REPORT_SIZE:
        raise HidProtocolError(f"bad HID payload size: {len(payload)}")
    return payload


def decode_frame(data: Iterable[int] | bytes | bytearray) -> tuple[int, int, int, int, bytes]:
    payload = normalize_report(data)
    kind, msg_id, index, total = payload[:4]
    if total == 0 or total > MAX_FRAGMENTS or index >= total:
        raise HidProtocolError(f"invalid fragment header kind={kind!r} id={msg_id} index={index} total={total}")
    return kind, msg_id, index, total, payload[HEADER_SIZE:]


class MessageAssembler:
    """Reassemble reports while tolerating interleaved logs/events."""

    def __init__(self) -> None:
        self._active: dict[tuple[int, int], tuple[int, int, bytearray]] = {}
        self.dropped = 0

    def feed(self, data: Iterable[int] | bytes | bytearray) -> Optional[HidMessage]:
        try:
            kind, msg_id, index, total, body = decode_frame(data)
        except HidProtocolError:
            self.dropped += 1
            return None
        key = (kind, msg_id)
        if index == 0:
            self._active[key] = (total, 0, bytearray())
        state = self._active.get(key)
        if state is None:
            self.dropped += 1
            return None
        expected_total, expected_index, buf = state
        if expected_total != total or expected_index != index:
            self._active.pop(key, None)
            self.dropped += 1
            return None
        buf.extend(body)
        next_index = index + 1
        if next_index != total:
            self._active[key] = (total, next_index, buf)
            return None
        self._active.pop(key, None)
        # Fixed-size reports are NUL padded by the sender.  Firmware commands
        # are text, so only remove terminal padding, never interior NULs.
        text = bytes(buf).rstrip(b"\x00").decode("utf-8", "replace")
        return HidMessage(kind, msg_id, text, total)


def _require_hid(module: Any = None) -> Any:
    module = module or _hid
    if module is None:
        raise HidUnavailable("hidapi is not installed; install tools/requirements-hid.txt")
    return module


def enumerate_candidates(module: Any = None, vid: int = VID, pid: int = PID) -> list[dict[str, Any]]:
    module = _require_hid(module)
    items = list(module.enumerate(vid, pid))
    # The product has CDC interfaces plus two vendor-defined HID collections.
    # Prefer interface 2, but do not rely on that number across OS backends.
    preferred = [item for item in items if item.get("interface_number") == 2]
    if preferred:
        return preferred
    return [item for item in items if item.get("usage_page", 0) in (0xFF00, 0xFF01, 0)] or items


def _path_label(path: Any) -> str:
    if isinstance(path, bytes):
        return path.decode(errors="replace")
    return str(path)


class HidLink:
    """Synchronous host-side meo_link client."""

    def __init__(self, info: dict[str, Any], *, module: Any = None, verbose: bool = False) -> None:
        module = _require_hid(module)
        self.info = info
        self.verbose = verbose
        self.dev = module.device()
        self.dev.open_path(info["path"])
        self.dev.set_nonblocking(0)
        self.assembler = MessageAssembler()
        self.next_id = 0
        self.rx_messages = 0
        self.rx_logs = 0
        self.rx_events = 0

    def close(self) -> None:
        try:
            self.dev.close()
        except Exception:
            pass

    def send_line(self, text: str) -> int:
        frames = fragment_message(text, KIND_COMMAND, self.next_id)
        msg_id = self.next_id
        self.next_id = (self.next_id + 1) & 0xFF
        for frame in frames:
            # hidapi writes include the report ID as the first byte.
            written = self.dev.write(bytes([REPORT_ID]) + frame)
            if written <= 0:
                raise HidProtocolError(f"HID write failed for fragment {msg_id}")
        return msg_id

    def read_message(self, timeout_ms: int = 500) -> Optional[HidMessage]:
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            remaining = int(max(0, (deadline - time.monotonic()) * 1000))
            if remaining <= 0:
                return None
            data = self.dev.read(REPORT_SIZE + 1, remaining)
            if not data:
                return None
            message = self.assembler.feed(data)
            if message is None:
                continue
            self.rx_messages += 1
            if message.kind == KIND_LOG:
                self.rx_logs += 1
            elif message.kind == KIND_EVENT:
                self.rx_events += 1
            if self.verbose:
                print(f"  [{message.kind_name}] {message.text.rstrip()[:160]}")
            return message

    def command(self, text: str, *, expected: str = "", timeout_ms: int = 1800,
                collect_ms: int = 0) -> CommandResult:
        started = time.monotonic()
        result = CommandResult(text, False, 0)
        try:
            self.send_line(text)
            deadline = started + timeout_ms / 1000.0
            matched = False
            while time.monotonic() < deadline:
                message = self.read_message(min(350, max(1, int((deadline - time.monotonic()) * 1000))))
                if message is None:
                    break
                if message.kind == KIND_LOG:
                    result.logs.append(message.text)
                    continue
                if message.kind == KIND_EVENT:
                    result.events.append(message.text)
                    continue
                if message.kind != KIND_REPLY:
                    continue
                result.replies.append(message.text)
                if not expected or expected in message.text:
                    matched = True
                    if collect_ms <= 0:
                        break
                    deadline = max(deadline, time.monotonic() + collect_ms / 1000.0)
            result.ok = matched
            if not matched:
                result.error = f"timeout waiting for {expected or 'reply'}"
        except Exception as exc:
            result.error = f"{type(exc).__name__}: {exc}"
        result.elapsed_ms = int((time.monotonic() - started) * 1000)
        return result


def probe_link(*, module: Any = None, verbose: bool = False,
               vid: int = VID, pid: int = PID) -> tuple[dict[str, Any], HidLink]:
    """Find the command collection by issuing a harmless LINK CAPS probe."""
    candidates = enumerate_candidates(module, vid=vid, pid=pid)
    failures: list[str] = []
    for info in candidates:
        try:
            link = HidLink(info, module=module, verbose=verbose)
            result = link.command(f"LINK CAPS {_token('p')}", expected="caps=1", timeout_ms=800)
            if result.ok:
                return info, link
            failures.append(f"{_path_label(info.get('path'))}: {result.error}")
            link.close()
        except Exception as exc:
            failures.append(f"{_path_label(info.get('path'))}: {exc}")
    suffix = "; ".join(failures[-3:])
    raise HidProtocolError(f"no Meoave HID command collection found ({suffix})")


def _format_spec(spec: CommandSpec, token: str) -> str:
    return spec.command.format(token=token)


def run_suite(link: HidLink, suite: str, *, allow_side_effects: bool = False,
              verbose: bool = False) -> dict[str, Any]:
    if suite not in ("safe", "full"):
        raise ValueError("suite must be safe or full")
    selected = [spec for spec in COMMANDS if spec.category == "safe" or (suite == "full" and spec.category == "functional")]
    rows: list[dict[str, Any]] = []
    for spec in selected:
        if spec.side_effect and not allow_side_effects:
            rows.append({"name": spec.name, "command": spec.command, "status": "SKIP",
                         "reason": "requires --allow-side-effects"})
            continue
        token = _token("t")
        command = _format_spec(spec, token)
        result = link.command(command, expected=spec.expected, timeout_ms=spec.timeout_ms,
                              collect_ms=spec.collect_ms)
        status = "PASS" if result.ok else "FAIL"
        rows.append({"name": spec.name, "command": command, "status": status,
                     "elapsed_ms": result.elapsed_ms, "replies": result.replies,
                     "logs": result.logs, "events": result.events, "error": result.error})
        print(f"{status:<4} {spec.name:<20} {result.first_reply.rstrip()[:120] if result.first_reply else result.error}")
        if not result.ok and suite == "safe":
            # Safe acceptance must report all independent failures, not stop at
            # the first one; a flaky log stream should not hide later commands.
            continue
    passed = sum(row["status"] == "PASS" for row in rows)
    failed = sum(row["status"] == "FAIL" for row in rows)
    skipped = sum(row["status"] == "SKIP" for row in rows)
    return {"suite": suite, "passed": passed, "failed": failed, "skipped": skipped,
            "dropped_frames": link.assembler.dropped, "commands": rows,
            "protocol": {"vid": f"0x{VID:04x}", "pid": f"0x{PID:04x}",
                          "report_id": REPORT_ID, "report_size": REPORT_SIZE,
                          "payload_size": PAYLOAD_SIZE}}


def write_report(path: str | Path, report: dict[str, Any]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _list_commands() -> int:
    for spec in COMMANDS:
        flags = ""
        if spec.side_effect:
            flags += " side-effect"
        if spec.destructive:
            flags += " DESTRUCTIVE"
        print(f"{spec.name:<20} [{spec.category}{flags}] {spec.command}")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vid", type=lambda x: int(x, 0), default=VID)
    parser.add_argument("--pid", type=lambda x: int(x, 0), default=PID)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--list", action="store_true", help="list registered command coverage")
    parser.add_argument("--command", help="send one raw command over HID")
    parser.add_argument("--suite", choices=("safe", "full"), help="run the acceptance suite")
    parser.add_argument("--allow-side-effects", action="store_true",
                        help="allow functional checks such as scan, UI and audio")
    parser.add_argument("--report", help="write a JSON result report")
    parser.add_argument("--timeout-ms", type=int, default=1800)
    args = parser.parse_args(argv)

    if args.list:
        return _list_commands()
    if not args.command and not args.suite:
        parser.error("choose --list, --command, or --suite")
    if _hid is None:
        print("FAIL hidapi is not installed; install tools/requirements-hid.txt", file=sys.stderr)
        return 2
    try:
        info, link = probe_link(module=_hid, verbose=args.verbose, vid=args.vid, pid=args.pid)
    except Exception as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    report: dict[str, Any] = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "device": {k: v for k, v in info.items() if k != "path"},
        "path": _path_label(info.get("path")),
    }
    try:
        if args.command:
            result = link.command(args.command, timeout_ms=args.timeout_ms)
            print(result.first_reply.rstrip() if result.first_reply else result.error)
            report["command"] = {"command": args.command, "ok": result.ok,
                                  "elapsed_ms": result.elapsed_ms, "replies": result.replies,
                                  "logs": result.logs, "events": result.events,
                                  "error": result.error}
            exit_code = 0 if result.ok else 1
        else:
            suite = run_suite(link, args.suite, allow_side_effects=args.allow_side_effects,
                               verbose=args.verbose)
            report.update(suite)
            exit_code = 0 if suite["failed"] == 0 else 1
    finally:
        link.close()
    if args.report:
        write_report(args.report, report)
        print(f"report: {Path(args.report)}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
