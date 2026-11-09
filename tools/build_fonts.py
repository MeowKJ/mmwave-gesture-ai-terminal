#!/usr/bin/env python3
"""Regenerate the LVGL font set in firmware/p4_gesture/main/fonts.

The manifest next to the generated fonts is the single source of truth for the
family, the source file, the licence and the exact character sets. Run:

  python tools/build_fonts.py                 # regenerate the current set
  python tools/build_fonts.py --set wenkai    # switch to another cute set
  python tools/build_fonts.py --size 44       # only one size
  python tools/build_fonts.py --check         # verify the TTF matches the manifest

The source TTF/OTF itself stays out of Git (build_ui_assets/ is ignored); the
manifest records its upstream URL and SHA-256 so the set is reproducible.
lv_font_conv is expected in firmware/p4_gesture/build_ui_assets/node_modules
(see package.json in that directory).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
P4 = ROOT / "firmware" / "p4_gesture"
ASSETS = P4 / "build_ui_assets"
FONT_DIR = P4 / "main" / "fonts"
MANIFEST = FONT_DIR / "manifest.json"

# 已验证过的整套字体：圆润清晰（默认）、手写文楷、随性手写。
# 三者都覆盖当前界面用到的全部汉字，切换后跑一遍桌面回归即可。
SETS = {
    "chill": {
        "family": "寒蝉圆黑体 ChillRoundGothic Medium",
        "file": "ChillRoundGothic_Medium.ttf",
        "source": "https://github.com/Warren2060/ChillRoundGothic (ttf/ChillRoundGothic_Medium.ttf)",
        "note": "思源黑体圆角化，稳重的圆润感，最保守",
    },
    "wenkai": {
        "family": "霞鹜文楷 LXGW WenKai Medium",
        "file": "LXGWWenKai-Medium.ttf",
        "source": "https://github.com/lxgw/LxgwWenKai/releases (v1.530 LXGWWenKai-Medium.ttf)",
        "note": "手写书本感，最治愈，但笔画偏细",
    },
    "yozai": {
        "family": "悠哉字体 Yozai Medium",
        "file": "Yozai-Medium.ttf",
        "source": "https://github.com/lxgw/yozai-font/releases (v0.868 Yozai-Medium.ttf)",
        "note": "随性手写，最俏皮，小字号略随意",
    },
    "maiyuan": {
        "family": "荆南麦圆体 KN Maiyuan",
        "file": "KNMaiyuan-Regular.ttf",
        "source": "https://github.com/maoken-fonts/KNMaiyuan (fonts/TTF/KNMaiyuan-Regular.ttf)",
        "note": "手写圆体，圆润俏皮最可爱，28px 仍然清楚",
    },
    "round": {
        "family": "资源圆体 Resource Han Rounded SC Medium",
        "file": "ResourceHanRoundedSC-Medium.ttf",
        "source": "https://github.com/CyanoHao/Resource-Han-Rounded (v0.990 RHR-TTF)",
        "note": "思源黑体全圆角，笔画端头圆润，最“圆圆”的一版",
    },
    "round_bold": {
        "family": "资源圆体 Resource Han Rounded SC Bold",
        "file": "ResourceHanRoundedSC-Bold.ttf",
        "source": "https://github.com/CyanoHao/Resource-Han-Rounded (v0.990 RHR-TTF)",
        "note": "同上但更粗，胖圆明显，小字略重",
    },
}


def load_manifest() -> dict:
    with MANIFEST.open(encoding="utf-8") as fh:
        return json.load(fh)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_font(manifest: dict) -> Path:
    name = manifest.get("file", "")
    for base in (ASSETS / "fonts-src", ASSETS):
        candidate = base / name
        if name and candidate.is_file():
            return candidate
    raise SystemExit(
        f"找不到字体源文件 {name}；请按 manifest.json 的 source 下载后放到 {ASSETS / 'fonts-src'}"
    )


def converter() -> Path:
    exe = ASSETS / "node_modules" / ".bin" / ("lv_font_conv.cmd" if os.name == "nt" else "lv_font_conv")
    if exe.is_file():
        return exe
    found = shutil.which("lv_font_conv")
    if found:
        return Path(found)
    raise SystemExit(
        "找不到 lv_font_conv；在 firmware/p4_gesture/build_ui_assets 里执行 npm install"
    )


def build(size: int, manifest: dict, font: Path, exe: Path) -> Path:
    key = f"symbols_{size}"
    symbols = manifest.get("sizes_symbols", {}).get(str(size)) or manifest.get(key) or manifest["symbols"]
    output = FONT_DIR / f"meo_font_{size}.c"
    command = [
        str(exe),
        "--font", str(font),
        "--size", str(size),
        "--bpp", str(manifest.get("bpp", 4)),
        "--format", "lvgl",
        "--range", "32-126",
        "--no-compress",
        "--no-kerning",
        "--lv-font-name", f"meo_font_{size}",
        "--output", str(output),
        "--symbols", symbols,
    ]
    print(f"\n== meo_font_{size} · {len(symbols)} symbols ==")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"lv_font_conv 失败（size {size}）")
    for line in (result.stdout + result.stderr).splitlines():
        if "arn" in line or "issing" in line or "rror" in line:
            print("  " + line.strip())
    print(f"  -> {output.relative_to(ROOT)}  {output.stat().st_size / 1024:.0f} KiB")
    return output


def apply_set(manifest: dict, name: str) -> dict:
    spec = SETS[name]
    manifest = dict(manifest)
    manifest["family"] = spec["family"]
    manifest["file"] = spec["file"]
    manifest["source"] = spec["source"]
    manifest["note"] = spec["note"]
    font = source_font(manifest)
    manifest["source_sha256"] = sha256(font)
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"切换字体：{name} · {spec['family']}")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="regenerate the Meoave LVGL font set")
    parser.add_argument("--size", type=int, action="append", help="only this pixel size (repeatable)")
    parser.add_argument("--set", choices=sorted(SETS), help="switch to another verified font set first")
    parser.add_argument("--list", action="store_true", help="list the verified font sets")
    parser.add_argument("--check", action="store_true", help="only verify the source font hash")
    args = parser.parse_args()

    if args.list:
        current = load_manifest().get("file")
        for name, spec in SETS.items():
            mark = "*" if spec["file"] == current else " "
            print(f"{mark} {name:8} {spec['family']}\n           {spec['note']}")
        return 0

    manifest = apply_set(load_manifest(), args.set) if args.set else load_manifest()
    font = source_font(manifest)
    actual = sha256(font)
    expected = manifest.get("source_sha256", "")
    print(f"source : {font}")
    print(f"sha256 : {actual}")
    if expected and actual != expected:
        print(f"expect : {expected}")
        raise SystemExit("字体源文件与 manifest.json 记录的 SHA-256 不一致")
    if args.check:
        print("ok")
        return 0

    sizes = [int(s) for s in manifest["sizes"]]
    if args.size:
        unknown = [s for s in args.size if s not in sizes]
        if unknown:
            raise SystemExit(f"manifest 里没有这些字号: {unknown}")
        sizes = args.size

    exe = converter()
    for size in sizes:
        build(size, manifest, font, exe)
    print("\n完成。桌面回归：")
    print("  cmake --build firmware/p4_gesture/tests/desktop/build -j 8")
    print("  firmware/p4_gesture/tests/desktop/build/meoave_visual_test.exe firmware/p4_gesture/test-results/ui-desktop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
