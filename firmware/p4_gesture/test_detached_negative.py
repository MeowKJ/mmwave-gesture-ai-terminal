"""Negative validation with the speaker physically disconnected.

With no speaker there is no acoustic path, so the loopback self test MUST
fail with "no acoustic tone". If it passed, a chip-internal path or electrical
crosstalk would be masquerading as a microphone signal - exactly the failure
mode this whole refactor was built to exclude.

This mode deliberately avoids long/repeated playbacks and verifies the
quiet 20% test level through register readback instead of loud sweeps.
"""
import re
import secrets
import sys
import time

import flash_usb


def connect():
    for _ in range(70):
        port = flash_usb.app_port()
        if port:
            try:
                return flash_usb.open_native(port.device, flash_usb.APP_PIDS)
            except Exception:
                pass
        time.sleep(0.4)
    return None


def read(stream, seconds, prefix=None, token=None):
    out, buf = [], b""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            chunk = stream.read(4096)
        except Exception:
            return out
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode(errors="replace").rstrip()
            if not text or (prefix and prefix not in text):
                continue
            if token and ("token=%s " % token) not in text:
                continue
            out.append(text)
    return out


def send(stream, text, wait=0.3, prefix=None, token=None):
    stream.write((text + "\n").encode())
    stream.flush()
    return read(stream, wait, prefix=prefix, token=token)


def tok():
    return secrets.token_hex(6)


def status(stream):
    t = tok()
    lines = send(stream, "AUDIO STATUS %s" % t, 0.9, "MEO:AUDIO_LOOP", t)
    if not lines:
        return {}
    body = lines[0].split("token=%s " % t, 1)[1]
    return dict(re.findall(r"(\w+)=([^\s]+)", body))


def main():
    stream = connect()
    if not stream:
        print("FATAL: no application port")
        return 2
    stream.reset_input_buffer()
    time.sleep(1.5)
    gates = []

    def gate(name, ok, detail=""):
        gates.append((name, bool(ok)))
        print("  [%s] %-40s %s" % ("PASS" if ok else "FAIL", name, detail))

    print("=== 1. static health ===")
    st = status(stream)
    print("   ", {k: st.get(k) for k in ("codec", "addr", "mclk", "vol", "reg", "route", "mic")})
    gate("codec present @0x12", st.get("codec") == "1" and st.get("addr") == "0x12")
    gate("MCLK pad alive", int(st.get("mclk", "0").rstrip("%")) >= 30, st.get("mclk"))
    gate("user volume still 50%", st.get("vol") == "50", "vol=%s" % st.get("vol"))
    gate("mic route 6/5", st.get("route") == "6/5", st.get("route"))

    print("\n=== 2. quiet test level + perceptual curve (register readback, no sound) ===")
    regs = []
    for pct in (20, 50, 90):
        send(stream, "SPK VOL %d" % pct, 0.25)
        time.sleep(0.15)
        r = status(stream).get("reg", "0x0")
        regs.append(int(r, 16))
        print("    vol=%3d%% -> DAC reg %s (%d)" % (pct, r, int(r, 16)))
    gate("volume codes monotonic", regs[0] < regs[1] < regs[2], str(regs))
    gate("20%% test level is 0x82", regs[0] == 0x82, hex(regs[0]))
    send(stream, "SPK VOL 50", 0.25)
    time.sleep(0.2)

    print("\n=== 3. disconnected speaker must FAIL the physical loopback ===")
    t = tok()
    stream.write(("AUDIO LOOPBACK %s 500\n" % t).encode())
    stream.flush()
    lines = read(stream, 9.0, "MEO:AUDIO_LOOP", t)
    phases, verdict = {}, ""
    for line in lines:
        body = line.split("token=%s " % t, 1)[1]
        if body.startswith("phase="):
            m = re.match(r"phase=(\w+) l=(\d+) r=(\d+)", body)
            if m:
                phases[m.group(1)] = (int(m.group(2)), int(m.group(3)))
        elif body.startswith("verdict="):
            verdict = body
    print("   ", verdict)
    for k, v in phases.items():
        print("    %-9s L=%d R=%d" % (k, v[0], v[1]))
    gate("verdict is FAIL", "verdict=FAIL" in verdict)
    gate("reason = no acoustic tone", "no acoustic tone" in verdict)
    gate("test level reported as 20", "testvol=20" in verdict)
    gate("tone near noise floor", max(phases.get("tone", (9999, 9999))) < 60,
         str(phases.get("tone")))

    print("\n=== 4. UI still healthy, volume restored ===")
    for target in ("speaker", "settings", "eyes"):
        send(stream, "UI GO %s" % target, 0.2)
        time.sleep(0.6)
        page = None
        for attempt in range(2):
            for line in send(stream, "UI", 0.9, "MEO:UI"):
                m = re.search(r"page=(\w+)", line)
                if m:
                    page = m.group(1)
            if page is not None: break
            time.sleep(0.3)   # one CDC report can be lost during a reset
        gate("UI GO %s" % target, page == target, "page=%s" % page)
    fin = status(stream)
    gate("volume restored to 50%", fin.get("vol") == "50", "vol=%s" % fin.get("vol"))

    stream.close()
    passed = sum(1 for _, ok in gates if ok)
    print("\n=== SUMMARY: %d/%d gates passed (speaker detached) ===" % (passed, len(gates)))
    for name, ok in gates:
        if not ok:
            print("  FAILED:", name)
    print("\nReconnect the speaker and run test_production_acceptance.py for the positive path.")
    return 0 if passed == len(gates) else 1


if __name__ == "__main__":
    sys.exit(main())
