"""Final verification after fixing three real codec faults:
  1. GPIO3 (LP pad) MCLK output enable
  2. reg 0x02 = 0x00 (system clock from MCLK pin, not 64fs BCLK)
  3. regs 0x27/0x28/0x46/0x47 = 0xBF (0x00 was digital silence)

Strict criteria:
  - outsel3=26, en3=1, duty3 ~ 50%
  - reg 0x02 == 0x00, vol regs == 0xBF
  - idle ADC is real noise (RAW not all zeros, small nonzero RMS)
  - TONE makes RMS rise, but NEVER exactly 6363/9000 (that was loopback)
"""
import secrets
import sys
import time

import flash_usb


def tok():
    return secrets.token_hex(6)


def read_lines(s, secs):
    out, buf, dl = [], b"", time.monotonic() + secs
    while time.monotonic() < dl:
        c = s.read(4096)
        if c:
            buf += c
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1)
                out.append(l.decode(errors="replace").rstrip())
    return out


def raw(s, text, wait=0.4):
    s.write((text + "\n").encode())
    s.flush()
    return [l for l in read_lines(s, wait) if "MEO:" in l]


def mic(s, verb, args="", wait=0.4):
    return raw(s, f"MIC {verb} {tok()} {args}".strip(), wait)


def open_stable():
    for _ in range(40):
        p = flash_usb.app_port()
        if p:
            try:
                return flash_usb.open_native(p.device, flash_usb.APP_PIDS)
            except Exception:
                pass
        time.sleep(0.4)
    sys.exit("no stable port")


def status(s, tag):
    lines = [l for l in mic(s, "STATUS") if "running=" in l]
    print(f"  [{tag}]", *(lines or ["<no reply>"]), flush=True)
    return lines


with open_stable() as s:
    s.reset_input_buffer()
    for _ in range(30):
        if any("running=1" in l for l in mic(s, "STATUS")):
            break
        time.sleep(0.4)

    print("=== 1. MCLK pad state (0 = read-only) ===", flush=True)
    for l in mic(s, "PAD3", "0", wait=0.8):
        print("  ", l, flush=True)
    for l in mic(s, "CLK", wait=1.0):
        print("  ", l, flush=True)

    print("\n=== 2. codec registers (want 02=00, 27/28/46/47=BF) ===", flush=True)
    for l in mic(s, "REGS", wait=1.2):
        print("  ", l, flush=True)

    print("\n=== 3. idle ADC: must be real noise, not constant 0 ===", flush=True)
    status(s, "idle")
    for i in range(2):
        for l in mic(s, "RAW", wait=0.6):
            print("  ", l, flush=True)

    print("\n=== 4. TONE: acoustic response, must not be 6363 ===", flush=True)
    for l in raw(s, "SPK ON", 0.4):
        print("  ", l, flush=True)
    for l in raw(s, "SPK TONE 1000", 0.12):
        print("  ", l, flush=True)
    peak = 0
    for i in range(8):
        time.sleep(0.25)
        for l in status(s, f"t{i}"):
            for p in l.split():
                if p.startswith("l_rms="):
                    peak = max(peak, int(p.split("=")[1]))
    print(f"  >>> PEAK = {peak} (6363 = fake loopback, 100..5000 = real acoustic)", flush=True)
    for l in raw(s, "SPK OFF"):
        print("  ", l, flush=True)
