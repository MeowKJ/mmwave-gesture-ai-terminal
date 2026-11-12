"""Production acceptance for the audio subsystem + UI.

Checks, in order:
  1. audio health snapshot (codec, MCLK, volume curve, mic route)
  2. physical loopback repeated 3x (DAC-mute and PA-off must both kill the tone)
  3. perceptual volume curve is monotonic in the acoustic domain
  4. every UI page can be entered without a reset
  5. mic mux calibration finds the crossed-harness route and persists it in NVS
  6. final loopback still PASS after all of the above

Exit code 0 only when every gate passes.
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
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            chunk = stream.read(4096)
        except Exception:
            # USB re-enumeration during SYS RESTART closes the handle; that is
            # expected, not a device failure.
            return out
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode(errors="replace").rstrip()
            if not text:
                continue
            if prefix and prefix not in text:
                continue
            if token and ("token=%s " % token) not in text:
                continue
            out.append(text)
    return out


def send(stream, text, wait=0.35, prefix=None, token=None):
    stream.write((text + "\n").encode())
    stream.flush()
    return read(stream, wait, prefix=prefix, token=token)


def token():
    return secrets.token_hex(6)


def status(stream):
    t = token()
    lines = send(stream, "AUDIO STATUS %s" % t, 0.9, "MEO:AUDIO_LOOP", t)
    if not lines:
        return {}
    body = lines[0].split("token=%s " % t, 1)[1]
    fields = {}
    for k, v in re.findall(r"(\w+)=([^\s]+)", body):
        fields[k] = v
    return fields


def loopback(stream, ms=700):
    t = token()
    stream.write(("AUDIO LOOPBACK %s %d\n" % (t, ms)).encode())
    stream.flush()
    lines = read(stream, 10.0, "MEO:AUDIO_LOOP", t)
    phases, verdict = {}, None
    for line in lines:
        body = line.split("token=%s " % t, 1)[1]
        if body.startswith("phase="):
            m = re.match(r"phase=(\w+) l=(\d+) r=(\d+)", body)
            if m:
                phases[m.group(1)] = (int(m.group(2)), int(m.group(3)))
        elif body.startswith("verdict="):
            verdict = body
    return verdict or "", phases


def tone_peak(stream, volume, ms=1200):
    # SPK TONE <ms> <pct> sets the level only for this tone and restores it
    # afterwards, so the sweep tests the perceptual curve without leaving the
    # device at 90%.
    send(stream, "SPK TONE %d %d" % (ms, volume), 0.12)
    peak = 0
    for _ in range(int(ms / 150) + 2):
        time.sleep(0.15)
        for line in send(stream, "MIC STATUS %s" % token(), 0.15, "MEO:MIC"):
            m = re.search(r"l_rms=(\d+)", line)
            if m:
                peak = max(peak, int(m.group(1)))
    return peak


def ui_page(stream, target):
    send(stream, "UI GO %s" % target, 0.25)
    time.sleep(0.7)
    for line in send(stream, "UI", 1.0, "MEO:UI"):
        m = re.search(r"page=(\w+)", line)
        if m:
            return m.group(1)
    return None


def main():
    stream = connect()
    if not stream:
        print("FATAL: no application port")
        return 2
    stream.reset_input_buffer()
    time.sleep(1.5)

    results = []

    def gate(name, ok, detail=""):
        results.append((name, bool(ok)))
        print("  [%s] %-38s %s" % ("PASS" if ok else "FAIL", name, detail))

    print("=== 1. audio health ===")
    st = status(stream)
    print("   ", {k: st.get(k) for k in
                  ("codec", "addr", "mclk", "vol", "reg", "route", "pa", "mic")})
    gate("codec present @0x12", st.get("codec") == "1" and st.get("addr") == "0x12")
    gate("MCLK pad alive (>=30%)", int(st.get("mclk", "0").rstrip("%")) >= 30,
         "mclk=%s" % st.get("mclk"))
    gate("default volume 50%", st.get("vol") == "50", "vol=%s reg=%s" %
         (st.get("vol"), st.get("reg")))
    gate("calibrated mic route 6/5", st.get("route") == "6/5", "route=%s" % st.get("route"))

    print("\n=== 2. physical loopback x3 ===")
    for i in range(3):
        v, ph = loopback(stream, 700)
        tone = max(ph.get("tone", (0, 0))) if "tone" in ph else 0
        base = max(ph.get("baseline", (0, 0))) if "baseline" in ph else 0
        dacm = max(ph.get("dac_mute", (0, 0))) if "dac_mute" in ph else 0
        paof = max(ph.get("pa_off", (0, 0))) if "pa_off" in ph else 0
        ok = ("verdict=PASS" in v and tone > base * 3 and
              dacm * 100 < tone * 40 and paof * 100 < tone * 40)
        gate("loopback run %d" % (i + 1), ok,
             "tone=%d dac_mute=%d pa_off=%d" % (tone, dacm, paof))

    print("\n=== 3. perceptual volume curve (acoustic) ===")
    peaks = []
    for pct in (20, 50, 90):
        p = tone_peak(stream, pct)
        peaks.append(p)
        print("    vol=%3d%% -> mic peak RMS %d" % (pct, p))
    gate("volume is monotonic", peaks[0] < peaks[1] < peaks[2], str(peaks))

    print("\n=== 4. UI navigation ===")
    for target in ("speaker", "settings", "display", "accessibility", "mic", "speaker", "eyes"):
        got = ui_page(stream, target)
        gate("UI GO %s" % target, got == target, "page=%s" % got)

    print("\n=== 5. mic route calibration + NVS persistence ===")
    t = token()
    stream.write(("AUDIO CALIBRATE %s\n" % t).encode())
    stream.flush()
    cal = read(stream, 12.0, "MEO:AUDIO_LOOP", t)
    best = [l for l in cal if "verdict=BEST" in l]
    print("    ", best[0].split("token=%s " % t, 1)[1] if best else "<no result>")
    gate("calibration finds 6/5", bool(best) and "lsel=6 rsel=5" in best[0])
    gate("calibration saves 9 combinations", sum("cal lsel=" in l for l in cal) == 9)

    # Change the mux explicitly, let the adapter persist it, reboot, verify.
    send(stream, "MIC SEL %s 6 6 4" % token(), 0.3)
    time.sleep(1.2)
    gate("route change observed", status(stream).get("route") == "6/6")
    # Fire the reset without waiting for a reply: the CDC device disappears
    # immediately and any read here would be a false failure.
    try:
        stream.write(("SYS RESTART %s\n" % token()).encode())
        stream.flush()
    except Exception:
        pass
    try:
        stream.close()
    except Exception:
        pass
    time.sleep(4.5)
    stream = connect()
    if not stream:
        print("FATAL: device did not come back after restart")
        return 2
    stream.reset_input_buffer()
    time.sleep(1.5)
    after = status(stream)
    gate("route persisted across reboot", after.get("route") == "6/6",
         "route=%s" % after.get("route"))
    send(stream, "MIC SEL %s 6 5 4" % token(), 0.3)
    time.sleep(1.2)
    gate("route restored to 6/5", status(stream).get("route") == "6/5")

    print("\n=== 6. final loopback ===")
    v, ph = loopback(stream, 700)
    gate("final verdict PASS", "verdict=PASS" in v, v)

    stream.close()
    passed = sum(1 for _, ok in results if ok)
    print("\n=== SUMMARY: %d/%d gates passed ===" % (passed, len(results)))
    for name, ok in results:
        if not ok:
            print("  FAILED:", name)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
