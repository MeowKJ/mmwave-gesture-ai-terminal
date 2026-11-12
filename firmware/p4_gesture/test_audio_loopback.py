"""Production acoustic loopback regression for the speaker/microphone chain.

Runs the on-device self test (AUDIO LOOPBACK) and parses the structured
MEO:AUDIO_LOOP reply. A PASS proves the signal travelled through the
physical path DAC -> NS4150B -> speaker -> air -> microphone -> PGA -> ADC,
because the firmware also verifies that DAC digital mute and PA shutdown each
remove the tone (a chip-internal TX->RX loop would survive both).

Usage: python test_audio_loopback.py [tone_ms] [--calibrate]
"""
import re
import secrets
import sys
import time

import flash_usb


def open_stable():
    for _ in range(60):
        port = flash_usb.app_port()
        if port:
            try:
                return flash_usb.open_native(port.device, flash_usb.APP_PIDS)
            except Exception:
                pass
        time.sleep(0.4)
    sys.exit("no stable application port")


def collect(stream, token, seconds):
    lines, buf = [], b""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = stream.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip()
                if "MEO:AUDIO_LOOP" in text and ("token=%s " % token) in text:
                    lines.append(text.split("token=%s " % token, 1)[1].strip())
    return lines


def main():
    ms = 800
    calibrate = False
    for arg in sys.argv[1:]:
        if arg == "--calibrate":
            calibrate = True
        elif arg.isdigit():
            ms = int(arg)

    with open_stable() as stream:
        stream.reset_input_buffer()
        time.sleep(1.5)
        status_token = secrets.token_hex(6)
        stream.write(("AUDIO STATUS %s\n" % status_token).encode())
        stream.flush()
        print("status:", *collect(stream, status_token, 1.2), sep="\n  ")

        if calibrate:
            token = secrets.token_hex(6)
            stream.write(("AUDIO CALIBRATE %s\n" % token).encode())
            stream.flush()
            print("\ncalibration:")
            for line in collect(stream, token, 12.0):
                print("  ", line)
            return 0

        token = secrets.token_hex(6)
        stream.write(("AUDIO LOOPBACK %s %d\n" % (token, ms)).encode())
        stream.flush()
        print("\nloopback (tone %d ms):" % ms)
        lines = collect(stream, token, max(12.0, ms / 1000.0 + 9.0))
        for line in lines:
            print("  ", line)

        verdict = [l for l in lines if l.startswith("verdict=")]
        detail = [l for l in lines if l.startswith("phase=")]
        if not verdict:
            print("\nRESULT: NO VERDICT (device did not finish)")
            return 2
        ok = "verdict=PASS" in verdict[0]
        print("\nphases: %d, %s" % (len(detail), verdict[0]))
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
