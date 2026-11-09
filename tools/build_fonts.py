#!/usr/bin/env python3
"""Regenerate the LVGL font set in firmware/p4_gesture/main/fonts.

The manifest next to the generated fonts is the single source of truth for the
family, the source file, the licence and the exact character sets. Run:

  python tools/build_fonts.py                 # regenerate the current set
  python tools/build_fonts.py --fetch          # download the font bundle release
  python tools/build_fonts.py --set wenkai     # switch to another cute set
  python tools/build_fonts.py --size 44        # only one size
  python tools/build_fonts.py --check          # verify the TTF matches the manifest

The source TTF/OTF itself stays out of Git (build_ui_assets/ is ignored). The
manifest records the upstream URL, the SHA-256 of every file and the release
bundle that mirrors them, so the set is reproducible without hunting upstream
releases (Resource Han Rounded only ships a 7z). lv_font_conv is expected in
firmware/p4_gesture/build_ui_assets/node_modules (see package.json there).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
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
        "sha256": "7aaf168681f168b9f550734d9e25058ffe73213db07d7a50152d5446dd9e7094",
        "source": "https://github.com/Warren2060/ChillRoundGothic (ttf/ChillRoundGothic_Medium.ttf)",
        "note": "思源黑体圆角化，稳重的圆润感，最保守",
    },
    "wenkai": {
        "family": "霞鹜文楷 LXGW WenKai Medium",
        "file": "LXGWWenKai-Medium.ttf",
        "sha256": "f4b8a34d1f838fe411540cb7c97b1c384ac9738a876c4104500bc42508b2312a",
        "source": "https://github.com/lxgw/LxgwWenKai/releases (v1.530 LXGWWenKai-Medium.ttf)",
        "note": "手写书本感，最治愈，但笔画偏细",
    },
    "yozai": {
        "family": "悠哉字体 Yozai Medium",
        "file": "Yozai-Medium.ttf",
        "sha256": "05f50f252b45197d0d6148a4abc3e37c7cd4cfdf03c955b57c25d53d924f835f",
        "source": "https://github.com/lxgw/yozai-font/releases (v0.868 Yozai-Medium.ttf)",
        "note": "随性手写，最俏皮，小字号略随意",
    },
    "maiyuan": {
        "family": "荆南麦圆体 KN Maiyuan",
        "file": "KNMaiyuan-Regular.ttf",
        "sha256": "94ea78c51086b1ea58c1744784b2335dea2c5ef1c5193b80e02c7d46b8e0283f",
        "source": "https://github.com/maoken-fonts/KNMaiyuan (fonts/TTF/KNMaiyuan-Regular.ttf)",
        "note": "手写圆体，圆润俏皮最可爱，28px 仍然清楚",
    },
    "round": {
        "family": "资源圆体 Resource Han Rounded SC Medium",
        "file": "ResourceHanRoundedSC-Medium.ttf",
        "sha256": "64376d56c4aeb39f345417860b9fee3cac52accf2d64dd7cab63c4528fcdc2f7",
        "source": "https://github.com/CyanoHao/Resource-Han-Rounded (v0.990 RHR-TTF)",
        "note": "思源黑体全圆角，笔画端头圆润，最“圆圆”的一版",
    },
    "round_bold": {
        "family": "资源圆体 Resource Han Rounded SC Bold",
        "file": "ResourceHanRoundedSC-Bold.ttf",
        "sha256": "085a738ec5b4020607867b1dc9a65fd32b0465a74ac2a849ed4c579169e8bd28",
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
        f"找不到字体源文件 {name}；先执行 python tools/meoave.py fonts --fetch "
        f"（或按 manifest.json 的 source 手动下载到 {ASSETS / 'fonts-src'}）"
    )


def download(url: str, dest: Path, attempts: int = 4, headers: dict | None = None) -> Path:
    """带断点续传和重试的下载：GitHub 的 release CDN 会偶发 504。"""
    partial = dest.with_name(dest.name + ".part")
    for attempt in range(1, attempts + 1):
        have = partial.stat().st_size if partial.is_file() else 0
        request = urllib.request.Request(url)
        for key, value in (headers or {}).items():
            request.add_header(key, value)
        if have:
            request.add_header("Range", f"bytes={have}-")
        total = 0
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                resume = response.status == 206
                if not resume:
                    have = 0
                total = int(response.headers.get("Content-Length") or 0) + have
                with partial.open("ab" if resume else "wb") as fh:
                    for chunk in iter(lambda: response.read(1 << 20), b""):
                        fh.write(chunk)
                        have += len(chunk)
                        if total:
                            print(f"  {have * 100 // total:3d}%  {have / 1e6:5.1f}/{total / 1e6:.1f} MB", end="\r")
                print()
            if have and total and have >= total:
                partial.replace(dest)
                return dest
        except Exception as error:  # noqa: BLE001 - 网络错误一律重试
            print(f"  第 {attempt} 次中断（{error}），已收到 {have / 1e6:.1f} MB，重试")
            time.sleep(2 * attempt)
    raise RuntimeError(f"下载失败：{url}")


def bundle_sources(bundle: dict) -> list[tuple[str, dict]]:
    """直链 + GitHub API 资源地址两条通道，绕开偶发的 CDN 504。"""
    sources = [(bundle["url"], {})]
    api = bundle.get("api", "")
    name = bundle.get("asset", "")
    if api:
        try:
            with urllib.request.urlopen(api, timeout=60) as response:
                release = json.loads(response.read())
            for asset in release.get("assets", []):
                if asset.get("name") == name:
                    sources.append((asset["url"], {"Accept": "application/octet-stream"}))
                    break
        except Exception as error:  # noqa: BLE001 - 只是少一条备用通道
            print(f"  解析 API 备用地址失败（{error}），继续用直链")
    return sources


def fetch_bundle(manifest: dict) -> Path:
    """下载字体发布包，解压出 TTF/OTF 并逐个校验 SHA-256。"""
    bundle = manifest.get("bundle") or {}
    url = bundle.get("url", "")
    if not url:
        raise SystemExit("manifest.json 缺少 bundle.url，无法自动下载字体")
    target = ASSETS / "fonts-src"
    target.mkdir(parents=True, exist_ok=True)
    cache = ASSETS / "cache" / bundle.get("asset", Path(url).name)
    cache.parent.mkdir(parents=True, exist_ok=True)

    if cache.is_file() and sha256(cache) == bundle.get("sha256"):
        print(f"使用已缓存的字体包 {cache.name}")
    else:
        errors = []
        for source, headers in bundle_sources(bundle):
            print(f"下载字体包 {source}")
            try:
                download(source, cache, headers=headers)
            except RuntimeError as error:
                errors.append(str(error))
                continue
            if sha256(cache) == bundle.get("sha256"):
                break
            cache.unlink()
            errors.append(f"SHA-256 不一致：{source}")
        if not cache.is_file():
            raise SystemExit(
                "字体包下载失败（已尝试直链与 API）。可手动下载 "
                f"{url} 放到 {cache}，脚本会自动校验。\n  " + "\n  ".join(errors)
            )

    with zipfile.ZipFile(cache) as archive:
        sums_name = next((n for n in archive.namelist() if n.endswith("SHA256SUMS.txt")), "")
        if not sums_name:
            raise SystemExit("字体包里没有 SHA256SUMS.txt")
        sums = {}
        for line in archive.read(sums_name).decode("utf-8").splitlines():
            parts = line.split()
            if len(parts) == 2:
                sums[parts[1]] = parts[0]
        extracted = 0
        for name in archive.namelist():
            if not name.lower().endswith((".ttf", ".otf")):
                continue
            rel = name.split("/", 1)[1] if "/" in name else name
            if rel not in sums:
                continue
            data = archive.read(name)
            digest = hashlib.sha256(data).hexdigest()
            if digest != sums[rel]:
                raise SystemExit(f"{rel} 的 SHA-256 与包内清单不一致")
            (target / Path(rel).name).write_bytes(data)
            print(f"  ok  {Path(rel).name}  {len(data) / 1e6:.1f} MB")
            extracted += 1
    if not extracted:
        raise SystemExit("字体包里没有可用的字体文件")
    print(f"字体已就绪：{target}")
    return target


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
    actual = sha256(font)
    registered = spec.get("sha256")
    if registered and actual != registered:
        raise SystemExit(f"{spec['file']} 的 SHA-256 与登记值不一致（文件被替换或损坏）")
    manifest["source_sha256"] = actual
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"切换字体：{name} · {spec['family']}")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="regenerate the Meoave LVGL font set")
    parser.add_argument("--size", type=int, action="append", help="only this pixel size (repeatable)")
    parser.add_argument("--fetch", action="store_true", help="download the font bundle release and verify it")
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

    if args.fetch:
        fetch_bundle(load_manifest())
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
