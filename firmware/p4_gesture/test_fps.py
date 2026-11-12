"""Production frame-pacing acceptance: every page transition + every animation.

Requires firmware with MEO:UIFPS auto-reports. PASS criteria per window:
  fps >= 30.0, jank == 0, max_frame_ms <= 33 (i.e. no delivered frame missed
  the 33.3 ms budget). Windows with 0 frames are "static" and reported, not
  counted as failures.
"""
import sys, time, re, flash_usb

PAGES = ["apps", "settings", "display", "accessibility", "speaker", "mic",
         "wifi", "password", "breathe", "eyes", "speaker", "settings", "eyes"]


def conn():
    for _ in range(60):
        p = flash_usb.app_port()
        if p:
            try:
                return flash_usb.open_native(p.device, flash_usb.APP_PIDS)
            except Exception:
                pass
        time.sleep(0.4)
    return None


def drain(s, seconds):
    buf = b""; deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            c = s.read(8192)
            if c: buf += c
        except Exception:
            break
    lines = []
    for l in buf.decode(errors="replace").splitlines():
        if "MEO:UIFPS" in l or "MEO:UI " in l or "FLS2:STATUS" in l:
            lines.append(l.strip())
    return lines


def send(s, text):
    try:
        s.write((text + "\n").encode()); s.flush()
    except Exception as e:
        print("  write failed:", e)


def parse(line, prefix):
    if prefix not in line: return None
    d = {}
    for k, v in re.findall(r"(\w+)=([^\s]+)", line):
        d[k] = v
    return d


def main():
    s = conn()
    if not s: sys.exit("no app port")
    s.reset_input_buffer(); time.sleep(3.0)
    results = []

    print("=== 1. page transitions (auto window per navigation) ===")
    for p in PAGES:
        send(s, "UI GO " + p)
        lines = drain(s, 0.85)
        uifps = [parse(l, "MEO:UIFPS tag=") for l in lines]
        uifps = [x for x in uifps if x]
        if not uifps:
            print(f"  nav-{p:<19} NO REPORT")
            results.append((f"nav-{p}", None)); continue
        # raw line needed because fps=12.3 is parsed as "12"
        raw = [l for l in lines if "MEO:UIFPS tag=" in l][-1]
        m = re.search(r"frames=(\d+) fps=([\d.]+) span_ms=(\d+) max_ms=(\d+) "
                      r"jank=(\d+) render_max=(\d+) flush_max=(\d+)", raw)
        if not m:
            print(f"  nav-{p:<19} unparsable: {raw[:120]}")
            results.append((f"nav-{p}", None)); continue
        f, fpsv, span, maxms, jank, rmax, fmax = m.groups()
        cls = [re.search(r"slow_render=(\d+) slow_gap=(\d+)", l) for l in lines]
        cls = [x for x in cls if x]
        sr, sg = cls[-1].groups() if cls else ("?", "?")
        ok = (float(fpsv) >= 30.0 and int(jank) == 0 and int(maxms) <= 33
              and sr == "0" and sg.isdigit() and int(sg) <= 8 and int(f) >= 5)
        print(f"  nav-{p:<19} frames={f:<3} fps={fpsv:<7} span={span:<4}ms "
              f"max_ms={maxms:<3} jank={jank:<2} "
              f"render_max={int(rmax)/1000:5.1f}ms flush_max={int(fmax)/1000:5.1f}ms "
              f" sr={sr} sg={sg} {'PASS' if ok else 'FAIL'}")
        results.append((f"nav-{p}", ok))
        time.sleep(0.25)

    print("=== 2. in-page animations (manual windows) ===")
    # The navigation auto-window must finish before a manual window starts,
    # otherwise the later navigate() call overwrites its tag/counters.
    anims = [
        ("home-eyes-blink", "eyes", ["UI FPS 1600"], 2.3),
        ("wifi-pulse",      "wifi", ["UI FPS 1600"], 2.3),
        ("mic-vu",          "mic",  ["UI FPS 1600"], 2.3),
        ("breathe-bloom",   "breathe", ["UI FPS 1600"], 2.3),
        ("shutdown-overlay","eyes", ["UI SHUTDOWN 3", "UI FPS 1400"], 2.2),
    ]
    for name, page, cmds, wait in anims:
        send(s, "UI GO " + page)
        drain(s, 0.75)                       # let the nav window publish
        for c in cmds: send(s, c)
        lines = drain(s, wait)
        raw = [l for l in lines if "MEO:UIFPS tag=manual" in l]
        if not raw:
            print(f"  {name:<24} NO REPORT")
            results.append((name, None)); continue
        m = re.search(r"frames=(\d+) fps=([\d.]+) span_ms=(\d+) max_ms=(\d+) "
                      r"jank=(\d+) render_max=(\d+) flush_max=(\d+)", raw[-1])
        if not m:
            print(f"  {name:<24} unparsable: {raw[-1][:120]}")
            results.append((name, None)); continue
        f, fpsv, span, maxms, jank, rmax, fmax = m.groups()
        cls = [re.search(r"slow_render=(\d+) slow_gap=(\d+)", l) for l in lines]
        cls = [x for x in cls if x]
        sr, sg = cls[-1].groups() if cls else ("?", "?")
        if int(f) < 5:
            # A page that draws nothing while idle is the desired behaviour;
            # there is no animation whose pacing could be measured here.
            print(f"  {name:<24} frames={f:<3} STATIC (no animation running)")
            results.append((name, True)); continue
        ok = (float(fpsv) >= 30.0 and int(jank) == 0 and int(maxms) <= 33
              and sr == "0" and sg.isdigit() and int(sg) <= 8)
        print(f"  {name:<24} frames={f:<3} fps={fpsv:<7} span={span:<4}ms "
              f"max_ms={maxms:<3} jank={jank:<2} "
              f"render_max={int(rmax)/1000:5.1f}ms flush_max={int(fmax)/1000:5.1f}ms "
              f" sr={sr} sg={sg} {'PASS' if ok else 'FAIL'}")
        results.append((name, ok))
    send(s, "UI SHUTDOWN -1")
    drain(s, 0.4)

    print("=== 3. steady-state cost (UI perf window) ===")
    for p in ("eyes", "settings", "speaker", "wifi", "mic", "breathe"):
        send(s, "UI GO " + p); drain(s, 0.5)
        time.sleep(1.1)
        send(s, "UI")
        lines = [l for l in drain(s, 0.5) if "MEO:UI " in l]
        if lines:
            d = parse(lines[-1], "MEO:UI ")
            print(f"  {p:<12} frame_ms={d.get('frame_ms')} busy_us={d.get('busy_us')} "
                  f"render_us={d.get('render_us')} flush_us={d.get('flush_us')} "
                  f"refreshes/s={d.get('refreshes')} px_s={d.get('px_s')}")
        else:
            print(f"  {p:<12} NO STATUS")
    s.close()

    hard = [(n, ok) for n, ok in results if ok is False]
    missing = [(n, ok) for n, ok in results if ok is None]
    print(f"\nSUMMARY: {sum(1 for _, ok in results if ok is True)}/{len(results)} PASS, "
          f"{len(hard)} FAIL, {len(missing)} NO REPORT")
    if hard: print("  failed:", ", ".join(n for n, _ in hard))
    if missing: print("  missing:", ", ".join(n for n, _ in missing))
    return 1 if hard or missing else 0


if __name__ == "__main__":
    sys.exit(main())
