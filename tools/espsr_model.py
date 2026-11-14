"""ESP-SR WakeNet assets: local-only provisioning and honest build inspection.

No model download, phrase substitution, or training is performed here.
"""
from __future__ import annotations
import hashlib
import json
import re
import struct
from pathlib import Path

KEYWORD = "猫猫"
PARTITION_BYTES = 0xF0000


def wake_words(metadata: bytes) -> str:
    if not metadata or len(metadata) > 1024:
        raise ValueError("Invalid WakeNet metadata length")
    lines = metadata.decode("utf-8").strip().splitlines()
    info = next((line.strip() for line in lines if line and not line.startswith("#")), "")
    parts = info.split("_")
    if len(parts) < 6 or not parts[0].lower().startswith("wakenet9"):
        raise ValueError("Expected WakeNet9 metadata")
    return ";".join(parts[2::4])


def local_model(root: Path, name: str) -> dict:
    if not re.fullmatch(r"wn9[a-z0-9_]{1,27}", name):
        raise ValueError("Expected a WakeNet9 model name fitting the 32-byte pack field")
    root = root.resolve()
    if not root.is_dir():
        raise ValueError("ESP-SR model_dir does not exist")
    # A single reviewed asset per pack prevents accidentally flashing unrelated
    # models or overflowing the additive 960 KiB model partition.
    if sorted(p.name for p in root.iterdir()) != [name]:
        raise ValueError("model_dir must contain only the selected model folder")
    return model_files(root / name, name, KEYWORD)


def model_files(folder: Path, name: str, expected: str) -> dict:
    if not folder.is_dir() or folder.is_symlink():
        raise ValueError("Expected an ordinary model folder")
    names = ["_MODEL_INFO_", "wn9_data", "wn9_index"]
    if sorted(p.name for p in folder.iterdir()) != names:
        raise ValueError("Expected exactly _MODEL_INFO_, wn9_data and wn9_index")
    digest = hashlib.sha256()
    size = 4 + 36 + 40 * len(names)
    for item in names:
        path = folder / item
        if not path.is_file() or path.is_symlink() or not 0 < path.stat().st_size <= PARTITION_BYTES:
            raise ValueError("Invalid local model asset: " + item)
        data = path.read_bytes()
        if item == "_MODEL_INFO_" and wake_words(data) != expected:
            raise ValueError(f"Model keyword is not exactly {expected}; refusing substitution")
        size += len(data)
        digest.update(item.encode("ascii") + b"\0" + data)
    if size > PARTITION_BYTES:
        raise ValueError("Model exceeds the reserved 960 KiB partition; do not overwrite storage")
    return {"name": name, "keyword": expected, "sha256": digest.hexdigest(),
            "packed_bytes": size, "model_dir": folder.parent.as_posix()}


def configure(project: Path, build: Path, sdkconfig: Path, section: dict) -> tuple[Path | None, str]:
    """Apply only local model settings, leaving display/audio/USB policy alone."""
    preset = section.get("diagnostic_preset", "")
    if preset and preset != "hi_miaomiao":
        raise ValueError("Unknown ESP-SR diagnostic preset")
    if preset and section.get("model_dir"):
        raise ValueError("Choose diagnostic_preset OR a custom model_dir, not both")
    if not section:
        return None, ""
    if preset:
        name = "wn9_himiaomiao_tts"
        folder = project / "managed_components/espressif__esp-sr/model/wakenet_model" / name
        # On a clean checkout Component Manager downloads the pinned official
        # package during CMake configure. Its generated pack is checked again
        # after build; the local preset never invents or renames model bytes.
        info = model_files(folder, name, "Hi,喵喵") if folder.exists() else {
            "name": name, "keyword": "Hi,喵喵", "sha256": "official-preset-pending",
            "packed_bytes": 0, "model_dir": ""}
        info["diagnostic_preset"] = preset
    elif section.get("model_dir"):
        root = Path(section["model_dir"])
        root = root if root.is_absolute() else project.parents[1] / root
        info = local_model(root, section.get("model_name", "wn9_maomao"))
    else:
        # Clearing a local preset must also clear the cached sdkconfig; do
        # not silently ship the last diagnostic phrase as the product word.
        info = {"name": "wn9_maomao", "keyword": KEYWORD, "sha256": "unprovisioned",
                "model_dir": "", "packed_bytes": 4}
    build.mkdir(parents=True, exist_ok=True)
    manifest = build / ".meoave_espsr_model.json"
    previous = json.loads(manifest.read_text(encoding="utf-8")) if manifest.exists() else {}
    # Upstream's model pack rule depends only on sdkconfig, not external model
    # bytes. Explicitly invalidate only this generated image on asset changes.
    image = build / "srmodels/srmodels.bin"
    if previous.get("sha256") != info["sha256"] and image.is_file():
        image.unlink()
    manifest.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    policy = {
        "CONFIG_APP_MEOAVE_ESPSR_WAKE_MODEL": json.dumps(info["name"]),
        "CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD": json.dumps(info["keyword"], ensure_ascii=False),
        "CONFIG_ESP_SR_EXTERNAL_MODEL_PATH": json.dumps("" if preset else info["model_dir"], ensure_ascii=False),
        "CONFIG_SR_WN_WN9_HIMIAOMIAO_TTS": "y" if preset else "n",
    }
    fragment = build / "sdkconfig.espsr.defaults"
    content = "\n".join(f"{key}={value}" for key, value in policy.items()) + "\n"
    if not fragment.exists() or fragment.read_text(encoding="utf-8") != content:
        fragment.write_text(content, encoding="utf-8")
    if sdkconfig.exists():
        old = sdkconfig.read_text(encoding="utf-8")
        current = {}
        for line in old.splitlines():
            if line.startswith("CONFIG_") and "=" in line:
                key, value = line.split("=", 1)
                current[key] = value
            elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
                current[line.split()[1]] = "n"
        # IDF rewrites symbol ordering. Compare values, not the location of
        # our appended lines, or every subsequent build needlessly configures.
        if any(current.get(key) != value for key, value in policy.items()):
            lines = [line for line in old.splitlines()
                     if line.removeprefix("# ").split("=", 1)[0].split(" ", 1)[0] not in policy]
            new = "\n".join(lines) + "\n" + content
            sdkconfig.write_text(new, encoding="utf-8")
    return fragment, info["sha256"]


def inspect_image(path: Path, target: str, expected_keyword: str = KEYWORD) -> dict:
    """Inspect a generated pack. A non-empty pack alone is NOT target readiness."""
    if not path.is_file():
        return {"ready": False, "reason": "MODEL_IMAGE_MISSING"}
    data = path.read_bytes()
    if len(data) < 4 or len(data) > PARTITION_BYTES:
        raise ValueError("Invalid model pack length")
    count, = struct.unpack_from("<I", data)
    if not count:
        return {"ready": False, "reason": "MODEL_MISSING"}
    if count > 8:
        raise ValueError("Invalid model count")
    cursor, entries, models = 4, [], {}
    def name_at(pos):
        raw = data[pos:pos+32]
        if len(raw) != 32 or b"\0" not in raw or not raw[0]:
            raise ValueError("Invalid model/file name")
        return raw.split(b"\0", 1)[0].decode("utf-8")
    for _ in range(count):
        if cursor + 36 > len(data):
            raise ValueError("Truncated model header")
        name = name_at(cursor)
        files, = struct.unpack_from("<I", data, cursor + 32)
        cursor += 36
        if name in models or not 1 <= files <= 16:
            raise ValueError("Invalid model/file table")
        models[name] = {}
        for _ in range(files):
            if cursor + 40 > len(data):
                raise ValueError("Truncated file header")
            filename = name_at(cursor)
            offset, size = struct.unpack_from("<II", data, cursor + 32)
            if filename in models[name] or not size or offset + size > len(data):
                raise ValueError("Invalid file range")
            models[name][filename] = (offset, size)
            entries.append((offset, size))
            cursor += 40
    for i, (offset, size) in enumerate(entries):
        if offset < cursor or any(offset < a+b and a < offset+size for a,b in entries[:i]):
            raise ValueError("Overlapping model data")
    if target not in models:
        return {"ready": False, "reason": "MODEL_MISSING"}
    files = models[target]
    if not {"_MODEL_INFO_", "wn9_index", "wn9_data"} <= files.keys():
        raise ValueError("Incomplete WakeNet asset")
    offset, size = files["_MODEL_INFO_"]
    actual = wake_words(data[offset:offset+size])
    return {"ready": actual == expected_keyword, "reason": "READY" if actual == expected_keyword else "MODEL_WORD_MISMATCH",
            "model": target, "keyword": actual, "sha256": hashlib.sha256(data).hexdigest()}
