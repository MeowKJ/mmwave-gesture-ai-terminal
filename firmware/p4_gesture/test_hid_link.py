#!/usr/bin/env python3
"""Compatibility entry point for the desktop HID acceptance client.

The protocol implementation lives in ``tools/hid_command.py`` now.  Keep this
old path working for existing lab scripts, but do not maintain a second HID
framing implementation here.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from hid_command import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:] or ["--suite", "safe"]))
