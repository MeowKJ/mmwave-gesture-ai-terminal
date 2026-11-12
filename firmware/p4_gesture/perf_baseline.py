"""Baseline FPS probe: navigate every page, sample the UI perf window."""
import sys, time, re, statistics, flash_usb

PAGES = ["eyes", "apps", "settings", "wifi", "breathe",
         "display", "accessibility", "password", "mic", "speaker"]


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


KEYS = ("frame_ms", "busy_us", "render_us", "flush_us", "refreshes",
        "px_s", "flushes", "max_px", "frames")


def send(s, text, wait=0.45):
    try:
        s.write((text + "\n").encode()); s.flush()
    except Exception:
        return []
    buf = b""; deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        try:
            c = s.read(8192)
            if c: buf += c
        except Exception:
            break
    out = []
    for line in buf.decode(errors="replace").splitlines():
        if "MEO:UI " in line:
            d = {}
            for k, v in re.findall(r"(\w+)=([^\s]+)", line):
                if k in KEYS:
                    try: d[k] = int(v.rstrip(","))
                    except ValueError: pass
            m = re.search(r"page=(\w+)", line)
            d["page"] = m.group(1) if m else "?"
            out.append(d)
    return out


def main():
    s = conn()
    if not s:
        sys.exit("no app port")
    s.reset_input_buffer(); time.sleep(1.5)
    print("page         frame_ms  busy_us  render_us  flush_us  refreshes(≈fps)  px_s      max_px")
    for p in PAGES:
        send(s, "UI GO " + p, 0.25)
        time.sleep(1.3)                 # let a full 30-tick window settle
        rows = []
        for _ in range(3):
            rows += send(s, "UI", 0.35)
            time.sleep(0.35)
        if not rows:
            print(f"{p:<12} <no data>"); continue
        def med(k):
            v = [r[k] for r in rows if k in r]
            return int(statistics.median(v)) if v else -1
        print(f"{p:<12} {med('frame_ms'):>8} {med('busy_us'):>8} "
              f"{med('render_us'):>10} {med('flush_us'):>9} {med('refreshes'):>16} "
              f"{med('px_s'):>9} {med('max_px'):>9}")
    s.close()

if __name__ == "__main__":
    main()
