#!/usr/bin/env python3
"""Install the small host-side dependency set into the configured Python."""
from pathlib import Path
import subprocess

from meoave import load_config, python_command


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    interpreter = python_command(load_config())
    requirements = Path(__file__).resolve().parent / "requirements.txt"
    print(f"Installing host tools with {interpreter}")
    subprocess.run([interpreter, "-m", "pip", "install", "-r", str(requirements)],
                   cwd=ROOT, check=True)
    print("Host tools ready. Run: python tools/meoave.py doctor")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
