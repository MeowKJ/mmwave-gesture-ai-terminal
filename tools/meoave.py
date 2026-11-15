#!/usr/bin/env python3
"""Stable one-command build, test and native-USB workflow for Meoave.

Examples:
  python tools/meoave.py doctor
  python tools/meoave.py all
  python tools/meoave.py build --profile hosted
  python tools/meoave.py wifi
  python tools/meoave.py c5
  python tools/meoave.py fonts --set wenkai
  python tools/meoave.py hardware --cycles 1 --restarts 3
  python tools/meoave.py hid --suite safe

Machine-specific ESP-IDF paths belong in tools/config.toml, which is ignored
by Git. The tool also accepts the normal IDF_PATH/PATH environment setup.
"""
from __future__ import annotations

import argparse
import json
import hashlib
import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(__file__).resolve().parent
CONFIG_FILE = TOOLS / "config.toml"
P4_DEFAULT = ROOT / "firmware" / "p4_gesture"


def load_config() -> dict:
    if not CONFIG_FILE.exists():
        return {}
    with CONFIG_FILE.open("rb") as fh:
        return tomllib.load(fh)


def cfg(config: dict, section: str, key: str, default=""):
    return config.get(section, {}).get(key, default)


def resolve(value: str | Path, base: Path = ROOT) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (base / path).resolve()


def project_dir(config: dict) -> Path:
    return resolve(cfg(config, "project", "p4_dir", "firmware/p4_gesture"))


def console_uart_baud(build: Path) -> int:
    """Use the selected build's actual UART configuration, never a guessed baud."""
    config = json.loads((build / "config/sdkconfig.json").read_text(encoding="utf-8"))
    baud = int(config["ESP_CONSOLE_UART_BAUDRATE"])
    if baud not in (115200, 230400, 460800, 921600):
        raise ValueError(f"Unsupported maintenance UART baud: {baud}")
    return baud


def tool_path(config: dict, key: str, command: str) -> str:
    configured = cfg(config, "toolchain", key, "")
    if configured:
        path = resolve(configured, ROOT)
        if path.is_file():
            return str(path)
        if path.is_dir():
            candidate = path / (command + (".exe" if os.name == "nt" else ""))
            if candidate.is_file():
                return str(candidate)
    found = shutil.which(command)
    if found:
        return found
    return command


def environment(config: dict) -> dict:
    env = os.environ.copy()
    idf_path = cfg(config, "toolchain", "idf_path", "") or env.get("IDF_PATH", "")
    if idf_path:
        env["IDF_PATH"] = str(resolve(idf_path)) if not Path(idf_path).is_absolute() else idf_path
    idf_tools = cfg(config, "toolchain", "idf_tools_path", "") or env.get("IDF_TOOLS_PATH", "")
    if idf_tools:
        env["IDF_TOOLS_PATH"] = str(resolve(idf_tools)) if not Path(idf_tools).is_absolute() else idf_tools
    idf_version = cfg(config, "toolchain", "idf_version", "")
    if idf_version:
        env["ESP_IDF_VERSION"] = idf_version
    # The component manifest uses this variable to make ESP-SR opt-in.  A
    # missing variable is an error in Component Manager, so every managed
    # build gets an explicit value.  Set [features].espsr=true only after the
    # official esp-sr component and the custom WakeNet asset are installed.
    espsr = cfg(config, "features", "espsr", "0")
    env["MEOAVE_USE_ESP_SR"] = "1" if str(espsr).lower() in ("1", "true", "yes", "on") else "0"
    python_path = cfg(config, "toolchain", "python", "") or env.get("IDF_PYTHON_ENV_PATH", "")
    if python_path:
        path = Path(python_path)
        if path.name.lower().startswith("python"):
            env["IDF_PYTHON_ENV_PATH"] = str(path.parent.parent)
        else:
            env["IDF_PYTHON_ENV_PATH"] = str(path)
    prepend = []
    # ESP-SR's model packer invokes `python` by name. Use this build's selected
    # interpreter rather than whichever unrelated Python happens to be first.
    if python_path and Path(python_path).is_file():
        prepend.append(str(Path(python_path).parent))
    for key in ("compiler_bin", "riscv_bin", "cmake_bin", "ninja_bin"):
        value = cfg(config, "toolchain", key, "")
        if value:
            path = resolve(value)
            if path.is_dir():
                prepend.append(str(path))
    if prepend:
        env["PATH"] = os.pathsep.join(prepend + [env.get("PATH", "")])
    return env


def python_command(config: dict) -> str:
    value = cfg(config, "toolchain", "python", "")
    if value:
        path = resolve(value)
        if path.is_file():
            return str(path)
    return sys.executable


def run(command: list[str], cwd: Path, env: dict, title: str) -> None:
    print(f"\n== {title} ==")
    print("$ " + " ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def build_signature(config: dict, profile: str, build_dir: Path, sdkconfig: Path, defaults: str, env: dict) -> str:
    # Hash the defaults content: editing a CONFIG_* line must re-run configure,
    # otherwise the new symbol never reaches sdkconfig.<profile>.
    digest = hashlib.sha256()
    for name in defaults.split(";"):
        path = sdkconfig.parent / name.strip()
        if path.is_file():
            digest.update(path.read_bytes())
    sig = {
        "python": python_command(config),
        "cmake": tool_path(config, "cmake", "cmake"),
        "ninja": tool_path(config, "ninja", "ninja"),
        "esp_target": cfg(config, "toolchain", "esp_target", "esp32p4"),
        "sdkconfig": str(sdkconfig),
        "defaults": defaults,
        "defaults_sha": digest.hexdigest(),
        "profile": profile,
        "idf_path": env.get("IDF_PATH", ""),
        "idf_tools_path": env.get("IDF_TOOLS_PATH", ""),
        "esp_rom_elf_dir": env.get("ESP_ROM_ELF_DIR", ""),
        "espsr": env.get("MEOAVE_USE_ESP_SR", "0"),
        "espsr_asset_sha": env.get("MEOAVE_ESPSR_ASSET_SHA", ""),
    }
    return json.dumps(sig, sort_keys=True)


def programmer_payload_layout(payload: Path) -> str:
    """Validate every embedded byte; allow either normal provisioning or app-only diagnosis."""
    manifest = json.loads((payload.parent / "flasher_args.json").read_text(encoding="utf-8"))
    merged = payload.read_bytes()
    regions = []
    previous_end = 0
    entries = sorted(manifest["flash_files"].items(), key=lambda item: int(item[0], 0))
    if manifest.get("extra_esptool_args", {}).get("chip") != "esp32c5":
        raise RuntimeError("Expected ESP32-C5 payload")
    app_only = manifest.get("meoave_app_only")
    if app_only:
        if (len(entries) != 1 or int(entries[0][0], 0) != 0x20000 or
                app_only.get("mode") not in ("diagnostic", "restore") or
                app_only.get("partition_size") != 0x1e0000 or
                len(app_only.get("backup_sha256", "")) != 64):
            raise RuntimeError("Invalid app-only diagnostic/restore manifest")
    elif len(entries) != 4:
        raise RuntimeError("Expected exactly four ESP32-C5 provisioning regions")
    for raw_offset, filename in entries:
        offset = int(raw_offset, 0)
        data = (payload.parent / filename).read_bytes()
        size = len(data)
        if (offset < previous_end or size <= 0 or offset + size > len(merged) or
                offset % 4096 or size % 4 or merged[offset:offset+size] != data):
            raise RuntimeError("Invalid or stale C5 flash region layout/content")
        if app_only and size > 0x1e0000:
            raise RuntimeError("Application exceeds backed-up ota_0 partition")
        regions.append(f"    {{{offset}u, {size}u}},")
        previous_end = offset + size
    return ('#define C5_PAYLOAD_SHA "' + hashlib.sha256(merged).hexdigest() + '"\n'
            + f'#define C5_PAYLOAD_IS_DIAG {int(bool(app_only and app_only["mode"] == "diagnostic"))}\n'
            + 'static const struct { uint32_t offset, size; } c5_regions[] = {\n'
            + '\n'.join(regions) + '\n};\n')


def build(config: dict, profile: str, reconfigure: bool = False, c5_payload: Path | None = None) -> Path:
    p4 = project_dir(config)
    env = environment(config)
    if not env.get("ESP_ROM_ELF_DIR"):
        rom = esp_rom_elf_dir(config)
        if rom:
            env["ESP_ROM_ELF_DIR"] = rom
    jobs = int(cfg(config, "build", "jobs", 8))
    if not 1 <= jobs <= 64:
        raise ValueError("build.jobs must be between 1 and 64")
    cmake = tool_path(config, "cmake", "cmake")
    ninja = tool_path(config, "ninja", "ninja")
    if profile == "maintenance":
        build_name = cfg(config, "build", "maintenance_dir", "build_meoave_maintenance")
        defaults = "sdkconfig.defaults;sdkconfig.product.defaults;sdkconfig.hosted.defaults;sdkconfig.maintenance.defaults"
    elif profile == "hosted":
        build_name = cfg(config, "build", "hosted_dir", "build_meoave_hosted")
        defaults = cfg(config, "build", "hosted_defaults", "sdkconfig.defaults;sdkconfig.hosted.defaults")
    elif profile == "programmer":
        build_name = cfg(config, "build", "programmer_dir", "build_meoave_programmer")
        defaults = "sdkconfig.defaults;sdkconfig.product.defaults;sdkconfig.programmer.defaults"
    else:
        build_name = cfg(config, "build", "product_dir", "build_meoave")
        defaults = cfg(config, "build", "product_defaults", "sdkconfig.defaults;sdkconfig.product.defaults")
    if profile == "product" and "sdkconfig.hosted.defaults" not in defaults.split(";"):
        defaults += ";sdkconfig.hosted.defaults"
    build_dir = p4 / build_name
    sdkconfig = p4 / ("sdkconfig." + profile)
    if profile in ("product", "hosted") and env.get("MEOAVE_USE_ESP_SR") == "1":
        import espsr_model
        fragment, sha = espsr_model.configure(p4, build_dir, sdkconfig, config.get("espsr", {}))
        if fragment:
            defaults += ";" + str(fragment)
            env["MEOAVE_ESPSR_ASSET_SHA"] = sha
    # IDF defaults do not override existing sdkconfig values. Migrate the old
    # offline product once, preserving display/power/audio and build caches.
    if profile == "product" and sdkconfig.is_file():
        migrate_product_wifi(sdkconfig, p4 / "sdkconfig.hosted.defaults")
    extra_configure = []
    if profile == "programmer":
        payload = c5_payload or c5_dir(config) / cfg(config, "build", "c5_dir", "build_c5") / "c5_hosted_slave_merged.bin"
        payload = payload.resolve()
        if not payload.is_file():
            raise RuntimeError("Build the C5 payload first: python tools/meoave.py c5")
        header = programmer_payload_layout(payload)
        build_dir.mkdir(parents=True, exist_ok=True)
        layout = build_dir / "c5_payload_layout.h"
        if not layout.exists() or layout.read_text(encoding="utf-8") != header:
            layout.write_text(header, encoding="utf-8")
        extra_configure = [f"-DC5_PAYLOAD_FILE={payload}"]

    sig_file = build_dir / ".meoave_build_sig"
    expected_sig = build_signature(config, profile, build_dir, sdkconfig, defaults, env)
    if profile == "programmer":
        expected_sig += "\n" + str(payload)
    need_configure = (
        reconfigure
        or not (build_dir / "build.ninja").is_file()
        or not (build_dir / "CMakeCache.txt").is_file()
        or not sig_file.is_file()
    )
    if not need_configure:
        try:
            cached_sig = sig_file.read_text(encoding="utf-8").strip()
            need_configure = (cached_sig != expected_sig.strip())
        except OSError:
            need_configure = True

    if need_configure:
        run([cmake, "-S", str(p4), "-B", str(build_dir), "-G", "Ninja",
             f"-DPYTHON={python_command(config)}", "-DPYTHON_DEPS_CHECKED=1",
             "-DESP_PLATFORM=1", "-DCCACHE_ENABLE=False",
             f"-DIDF_TARGET={cfg(config, 'toolchain', 'esp_target', 'esp32p4')}",
             f"-DSDKCONFIG={sdkconfig}",
             f"-DSDKCONFIG_DEFAULTS={defaults}", *extra_configure], p4, env, f"configure {profile}")
        try:
            build_dir.mkdir(parents=True, exist_ok=True)
            sig_file.write_text(expected_sig, encoding="utf-8")
        except OSError:
            pass
    else:
        print(f"\n== configure {profile} (cached, skipped) ==")

    run([ninja, "-C", str(build_dir), "-j", str(jobs)], p4, env, f"build {profile}")
    verify_product_ui(build_dir, env)
    if profile in ("product", "hosted") and str(env.get("MEOAVE_USE_ESP_SR", "0")) == "1":
        verify_product_espsr(build_dir, env)
    if profile in ("product", "hosted"):
        verify_product_wifi(build_dir)
    return build_dir


def migrate_product_wifi(sdkconfig: Path, hosted_defaults: Path) -> None:
    current = sdkconfig.read_text(encoding="utf-8")
    if "CONFIG_APP_MEOAVE_WIFI_HOSTED=y" in current.splitlines():
        return
    # Only Wi-Fi/Hosted policy is migrated; diagnostic profiles are excluded.
    policy = [line for line in hosted_defaults.read_text(encoding="utf-8").splitlines()
              if line.startswith("CONFIG_") or line.startswith("# CONFIG_")]
    policy = [line for line in policy if any(prefix in line for prefix in
              ("CONFIG_ESP_HOSTED_", "CONFIG_ESP_WIFI_REMOTE_", "CONFIG_SLAVE_IDF_", "CONFIG_APP_MEOAVE_WIFI_"))]
    keys = {line.split("=", 1)[0].removeprefix("# ").split()[0] for line in policy}
    lines = [line for line in current.splitlines()
             if line.split("=", 1)[0].removeprefix("# ").split(" ", 1)[0] not in keys]
    sdkconfig.write_text("\n".join(lines + policy) + "\n", encoding="utf-8")
    print("Migrated legacy offline product: enabled C5 Wi-Fi (kept build cache)", flush=True)


def verify_product_wifi(build_dir: Path) -> None:
    path = build_dir / "config" / "sdkconfig.json"
    values = json.loads(path.read_text(encoding="utf-8"))
    required = ("APP_MEOAVE_WIFI_HOSTED", "ESP_WIFI_REMOTE_ENABLED", "ESP_HOSTED_ENABLED",
                "ESP_HOSTED_CP_TARGET_ESP32C5", "ESP_HOSTED_SDIO_HOST_INTERFACE")
    missing = [key for key in required if values.get(key) is not True]
    if missing or values.get("APP_C5_NATIVE_MAINTENANCE") or values.get("APP_C5_SDIO_LOADER"):
        raise RuntimeError("Product Wi-Fi is not linked/configured: " + ", ".join(missing))
    verify_wifi_abi(build_dir)
    print("PASS: product C5 Wi-Fi enabled (real scan callback, not offline UI)", flush=True)


def validate_wifi_abi(layouts: dict) -> None:
    required = ("bsp_wifi_hosted.c", "rpc_rsp.c", "rpc_wrap.c", "esp_wifi_weak.c")
    for name in required:
        if not layouts.get(name, {}).get("wifi_ap_record_t"):
            raise RuntimeError("Cannot verify Wi-Fi ABI: missing DWARF type in " + name)
    types = {name for unit in layouts.values() for name in unit}
    for name in sorted(types):
        found = {unit: types[name] for unit, types in layouts.items() if name in types}
        values = list(found.values())
        if any(value != values[0] for value in values[1:]):
            raise RuntimeError("Wi-Fi ABI mismatch for " + name + ": " + repr(found))


def verify_wifi_abi(build_dir: Path) -> dict:
    # Use the actual linked translation units, not a mock sizeof() assertion.
    # Native vs remote IDF headers once yielded 92/96-byte AP records here.
    from elftools.elf.elffile import ELFFile
    elfs = list(build_dir.glob("*.elf"))
    if len(elfs) != 1:
        raise RuntimeError("Cannot verify Wi-Fi ABI: expected one application ELF")
    units = ("bsp_wifi_hosted.c", "rpc_rsp.c", "rpc_wrap.c", "esp_wifi_weak.c")
    names = {b"wifi_ap_record_t", b"wifi_config_t", b"wifi_init_config_t",
             b"wifi_event_sta_scan_done_t", b"wifi_event_sta_connected_t",
             b"wifi_event_sta_disconnected_t"}
    layouts = {}
    with elfs[0].open("rb") as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            attr = cu.get_top_DIE().attributes.get("DW_AT_name")
            unit = Path(attr.value.decode(errors="replace")).name if attr else ""
            if unit not in units:
                continue
            found = {}
            for die in cu.iter_DIEs():
                name = die.attributes.get("DW_AT_name")
                if die.tag != "DW_TAG_typedef" or not name or name.value not in names:
                    continue
                struct = die.get_DIE_from_attribute("DW_AT_type")
                size = struct.attributes.get("DW_AT_byte_size")
                if not size:
                    continue
                members = []
                for member in struct.iter_children():
                    if member.tag != "DW_TAG_member":
                        continue
                    row = []
                    for key in ("DW_AT_name", "DW_AT_data_member_location", "DW_AT_bit_size", "DW_AT_bit_offset", "DW_AT_data_bit_offset"):
                        value = member.attributes.get(key)
                        value = value.value if value else None
                        row.append(value.decode(errors="replace") if isinstance(value, bytes) else value)
                    members.append(row)
                found[name.value.decode()] = {"size": size.value, "members": members}
            layouts[unit] = found
    validate_wifi_abi(layouts)
    (build_dir / "wifi_abi.json").write_text(json.dumps(layouts, indent=2), encoding="utf-8")
    size = layouts["bsp_wifi_hosted.c"]["wifi_ap_record_t"]["size"]
    print(f"PASS: Wi-Fi ABI identical in application/Hosted/RPC (AP record {size} bytes)", flush=True)
    return layouts


def verify_product_ui(build_dir: Path, env: dict) -> None:
    """Reject successful builds that garbage-collected the product UI entry."""
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    nm = next((line.split("=", 1)[1] for line in cache.splitlines()
               if line.startswith("CMAKE_NM:FILEPATH=")), "")
    elfs = list(build_dir.glob("*.elf"))
    if not nm or len(elfs) != 1:
        raise RuntimeError("Cannot verify product UI: expected CMAKE_NM and one application ELF")
    result = subprocess.run([nm, "--defined-only", str(elfs[0])],
                            env=env, check=True, capture_output=True, text=True)
    symbols = {line.split()[-1] for line in result.stdout.splitlines() if line.split()}
    missing = {"meoave_app_create", "meo_ui_create"} - symbols
    if missing:
        raise RuntimeError("Product UI missing from linked ELF: " + ", ".join(sorted(missing)))
    print("PASS: product UI entry and renderer present in final ELF", flush=True)


def verify_product_espsr(build_dir: Path, env: dict) -> None:
    """Verify that ESP-SR is linked without claiming a model is present."""
    config_path = build_dir / "config" / "sdkconfig.json"
    if not config_path.is_file():
        raise RuntimeError("Cannot verify ESP-SR: generated sdkconfig.json is missing")
    values = json.loads(config_path.read_text(encoding="utf-8"))
    if values.get("APP_MEOAVE_ESPSR") is not True:
        raise RuntimeError("ESP-SR adapter is disabled in the product configuration")
    if str(env.get("MEOAVE_USE_ESP_SR", "0")) != "1":
        print("WARN: ESP-SR component is configured but MEOAVE_USE_ESP_SR is 0", flush=True)
        return

    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    nm = next((line.split("=", 1)[1] for line in cache.splitlines()
               if line.startswith("CMAKE_NM:FILEPATH=")), "")
    elfs = list(build_dir.glob("*.elf"))
    if not nm or len(elfs) != 1:
        raise RuntimeError("Cannot verify ESP-SR: expected CMAKE_NM and one application ELF")
    result = subprocess.run([nm, "--defined-only", str(elfs[0])],
                            env=env, check=True, capture_output=True, text=True)
    symbols = {line.split()[-1] for line in result.stdout.splitlines() if line.split()}
    missing = {"meoave_espsr_init", "meo_wake_feed_audio", "esp_wn_handle_from_name"} - symbols
    if missing:
        raise RuntimeError("ESP-SR path missing from linked ELF: " + ", ".join(sorted(missing)))

    model = values.get("APP_MEOAVE_ESPSR_WAKE_MODEL", "")
    image = build_dir / "srmodels" / "srmodels.bin"
    import espsr_model
    keyword = values.get("APP_MEOAVE_ESPSR_WAKE_WORD", "猫猫")
    result = espsr_model.inspect_image(image, model, keyword)
    if result["ready"]:
        scope = "production target" if keyword == "猫猫" else "DIAGNOSTIC ONLY, not 猫猫"
        print(f"PASS: ESP-SR linked; exact {keyword} model packaged ({model}, {scope}); acoustic acceptance still required", flush=True)
    else:
        print(f"PASS: ESP-SR linked; WARN: {result['reason']} ({model}); acoustic wake remains disabled", flush=True)


def tests(config: dict) -> None:
    p4 = project_dir(config)
    run([python_command(config), "-m", "unittest", "discover", "-s", "tests",
         "-p", "test_*.py", "-v"], p4, environment(config), "python tests")
    run([python_command(config), "-m", "unittest", "discover", "-s", str(TOOLS),
         "-p", "test_hid_command.py", "-v"], ROOT, environment(config),
        "desktop HID protocol tests")


def wifi_status(config: dict, args: argparse.Namespace) -> int:
    """One explicit CMD5(arg=0) C5 SDIO probe. Never resets or flashes the C5.

    A completed probe is diagnostic only: no Hosted handshake, chip identity or
    Wi-Fi usability is implied by an R4 response (or by its absence).
    """
    p4 = project_dir(config)
    build_name = cfg(config, "build", "product_dir", "build_meoave")
    command = [python_command(config), "probe_wifi_sdio.py",
               "--build-dir", str(p4 / build_name),
               "--report", str(resolve(args.report, p4))]
    if args.no_reset:
        command.append("--no-reset")
    print("\n== C5 SDIO status probe (read-only) ==", flush=True)
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(command, cwd=p4, env=environment(config)).returncode


def c5_dir(config: dict) -> Path:
    return resolve(cfg(config, "project", "c5_dir", "firmware/c5_hosted"))


def esp_rom_elf_dir(config: dict) -> str:
    """IDF's gdbinit step needs ESP_ROM_ELF_DIR; derive it when unset."""
    tools_path = environment(config).get("IDF_TOOLS_PATH", "")
    if not tools_path:
        return ""
    root = Path(tools_path) / "esp-rom-elfs"
    candidates = sorted(p for p in root.glob("*") if p.is_dir()) if root.is_dir() else []
    return str(candidates[-1]) if candidates else ""


def merge_c5_image(config: dict, build_dir: Path) -> Path:
    """Fold the four images into one file using the offsets IDF just generated."""
    args_file = build_dir / "flash_args"
    flags: list[str] = []
    pairs: list[str] = []
    for line in args_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("--"):
            flags.extend(line.split())
        else:
            offset, name = line.split(None, 1)
            pairs.extend([offset, name.strip()])
    merged = build_dir / "c5_hosted_slave_merged.bin"
    command = [python_command(config), "-m", "esptool", "--chip",
               cfg(config, "toolchain", "c5_target", "esp32c5"), "merge-bin",
               "-o", str(merged), *flags, *pairs]
    run(command, build_dir, environment(config), "merge C5 flash image")
    print(f"merged image: {merged} ({merged.stat().st_size} bytes)")
    return merged


def c5_build(config: dict, args: argparse.Namespace) -> None:
    """Build the ESP32-C5 ESP-Hosted slave firmware (firmware/c5_hosted).

    The pinned slave carries app-only maintenance/rollback support. C5 GPIO2
    defaults high-Z; only the leased maintenance API may temporarily change it.
    """
    project = c5_dir(config)
    if not (project / "CMakeLists.txt").is_file():
        raise SystemExit(f"C5 slave project not found: {project}")
    env = environment(config)
    if not env.get("ESP_ROM_ELF_DIR"):
        rom = esp_rom_elf_dir(config)
        if rom:
            env["ESP_ROM_ELF_DIR"] = rom
    build_name = cfg(config, "build", "c5_dir", "build_c5")
    fault_test = getattr(args, "fault_test", False)
    hang_test = getattr(args, "hang_test", False)
    if fault_test and hang_test:
        raise ValueError("Select only one C5 failure-injection image")
    if fault_test:
        build_name += "_fault"
    elif hang_test:
        build_name += "_hang"
    build_dir = project / build_name
    defaults = cfg(config, "build", "c5_defaults",
                   "sdkconfig.defaults;sdkconfig.defaults.esp32c5")
    if fault_test:
        defaults += ";sdkconfig.fault.defaults"
    elif hang_test:
        defaults += ";sdkconfig.hang.defaults"
    sdkconfig_file = build_dir / "sdkconfig" if (fault_test or hang_test) else project / "sdkconfig.c5"
    configure = [tool_path(config, "cmake", "cmake"), "-S", str(project), "-B", str(build_dir),
                 "-G", "Ninja", f"-DPYTHON={python_command(config)}", "-DPYTHON_DEPS_CHECKED=1",
                 "-DESP_PLATFORM=1", "-DCCACHE_ENABLE=False",
                 f"-DIDF_TARGET={cfg(config, 'toolchain', 'c5_target', 'esp32c5')}",
                 f"-DSDKCONFIG={sdkconfig_file}",
                 f"-DSDKCONFIG_DEFAULTS={defaults}"]
    sig_file = build_dir / ".meoave_build_sig"
    # Editing sdkconfig.defaults must invalidate the cached configure step,
    # otherwise a newly added CONFIG_* never reaches sdkconfig.c5.
    defaults_sha = hashlib.sha256()
    for name in defaults.split(";"):
        path = project / name.strip()
        if path.is_file():
            defaults_sha.update(path.read_bytes())
    c5_sig = json.dumps({
        "python": python_command(config),
        "cmake": tool_path(config, "cmake", "cmake"),
        "ninja": tool_path(config, "ninja", "ninja"),
        "c5_target": cfg(config, "toolchain", "c5_target", "esp32c5"),
        "sdkconfig": str(sdkconfig_file),
        "defaults": defaults,
        "defaults_sha": defaults_sha.hexdigest(),
        "idf_path": env.get("IDF_PATH", ""),
        "idf_tools_path": env.get("IDF_TOOLS_PATH", ""),
        "esp_rom_elf_dir": env.get("ESP_ROM_ELF_DIR", ""),
    }, sort_keys=True)
    need_configure = (
        args.clean
        or not (build_dir / "build.ninja").is_file()
        or not (build_dir / "CMakeCache.txt").is_file()
        or not sig_file.is_file()
    )
    if not need_configure:
        try:
            cached_sig = sig_file.read_text(encoding="utf-8").strip()
            need_configure = (cached_sig != c5_sig.strip())
        except OSError:
            need_configure = True

    if need_configure:
        if args.clean:
            # Re-apply sdkconfig.defaults from scratch. cmake --fresh keeps the object
            # cache but drops CMakeCache.txt, so no bulk delete of the build tree.
            if sdkconfig_file.is_file():
                print(f"remove {sdkconfig_file}")
                sdkconfig_file.unlink()
            configure.insert(1, "--fresh")
        run(configure, project, env, "configure c5_hosted")
        try:
            build_dir.mkdir(parents=True, exist_ok=True)
            sig_file.write_text(c5_sig, encoding="utf-8")
        except OSError:
            pass
    else:
        print("\n== configure c5_hosted (cached, skipped) ==")
    jobs = int(cfg(config, "build", "jobs", 8))
    if not 1 <= jobs <= 64:
        raise ValueError("build.jobs must be between 1 and 64")
    run([tool_path(config, "ninja", "ninja"), "-C", str(build_dir), "-j", str(jobs)], project, env,
        "build c5_hosted")
    if not args.no_merge:
        merge_c5_image(config, build_dir)


def hardware(config: dict, args: argparse.Namespace) -> None:
    p4 = project_dir(config)
    build_name = cfg(config, "build", "product_dir", "build_meoave")
    build_dir = p4 / build_name
    if args.build:
        build(config, "product")
    command = [python_command(config), "usb_iterate.py", "--build-dir", str(build_dir),
               "--cycles", str(args.cycles), "--restarts", str(args.restarts),
               "--report", str(resolve(args.report, p4))]
    run(command, p4, environment(config), "native USB hardware acceptance")


def hid_acceptance(config: dict, args: argparse.Namespace) -> int:
    """Run the desktop HID command client without touching CDC or COM ports."""
    command = [python_command(config), str(TOOLS / "hid_command.py")]
    if args.list:
        command.append("--list")
    elif args.command:
        command += ["--command", args.command]
    else:
        command += ["--suite", args.suite]
        if args.allow_side_effects:
            command.append("--allow-side-effects")
    if args.verbose:
        command.append("--verbose")
    if args.report and not args.list:
        command += ["--report", str(resolve(args.report))]
    print("\n== desktop HID acceptance ==", flush=True)
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(command, cwd=ROOT, env=environment(config)).returncode


def doctor(config: dict) -> int:
    p4 = project_dir(config)
    print(f"root        : {ROOT}")
    print(f"p4 project  : {p4}")
    print(f"python      : {python_command(config)}")
    print(f"IDF_PATH    : {environment(config).get('IDF_PATH', '<unset>')}")
    idf = environment(config).get("IDF_PATH", "")
    checks = [(p4.is_dir(), "p4 project exists"),
              (bool(idf) and Path(idf).is_dir(), "IDF_PATH exists"),
              (shutil.which(tool_path(config, "cmake", "cmake")) is not None or Path(tool_path(config, "cmake", "cmake")).is_file(), "cmake available"),
              (shutil.which(tool_path(config, "ninja", "ninja")) is not None or Path(tool_path(config, "ninja", "ninja")).is_file(), "ninja available")]
    try:
        import serial  # noqa: F401
        checks.append((True, "pyserial import"))
    except ImportError:
        checks.append((False, "pyserial import"))
    failed = [name for ok, name in checks if not ok]
    for ok, name in checks:
        print(f"[{'OK' if ok else 'FAIL'}] {name}")
    if failed:
        print("\nFix paths in tools/config.toml or install tools/requirements.txt")
        return 1
    return 0


def clean(config: dict) -> None:
    p4 = project_dir(config).resolve()
    allowed = {p4 / cfg(config, "build", "product_dir", "build_meoave"),
               p4 / cfg(config, "build", "hosted_dir", "build_meoave_hosted")}
    for path in allowed:
        path = path.resolve()
        if path.parent != p4 or path == p4:
            raise RuntimeError(f"refusing unsafe clean path: {path}")
        if path.exists():
            print(f"remove {path}")
            shutil.rmtree(path)


def fonts(config: dict, args: argparse.Namespace) -> None:
    """Regenerate the LVGL font set from main/fonts/manifest.json."""
    command = [python_command(config), "tools/build_fonts.py"]
    if args.set:
        command += ["--set", args.set]
    if args.fetch:
        command.append("--fetch")
    if args.list:
        command.append("--list")
    if args.size:
        for size in args.size:
            command += ["--size", str(size)]
    if args.check:
        command.append("--check")
    run(command, ROOT, environment(config), "LVGL font set")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor")
    build_parser = sub.add_parser("build")
    build_parser.add_argument("--profile", choices=("product", "hosted", "programmer", "maintenance"), default="product")
    build_parser.add_argument("--reconfigure", action="store_true",
                              help="force CMake reconfigure even if build configuration is unchanged")
    sub.add_parser("test")
    all_parser = sub.add_parser("all", help="build product and run host tests")
    all_parser.add_argument("--hosted", action="store_true", help="also build the SDIO Hosted profile")
    all_parser.add_argument("--reconfigure", action="store_true",
                            help="force CMake reconfigure before building")
    hw = sub.add_parser("hardware", help="run the bounded native-USB acceptance flow")
    hw.add_argument("--build", action="store_true")
    hw.add_argument("--cycles", type=int, default=1)
    hw.add_argument("--restarts", type=int, default=3)
    hw.add_argument("--report", default="test-results/tools_usb_iteration.json")
    hid = sub.add_parser("hid", help="run the desktop HID command client and acceptance suites")
    hid_mode = hid.add_mutually_exclusive_group(required=True)
    hid_mode.add_argument("--list", action="store_true", help="list registered command coverage")
    hid_mode.add_argument("--command", help="send one raw command over HID")
    hid_mode.add_argument("--suite", choices=("safe", "full"), help="run a safe or functional suite")
    hid.add_argument("--allow-side-effects", action="store_true",
                     help="allow UI, Wi-Fi scan, audio and mic functional checks")
    hid.add_argument("--verbose", action="store_true")
    hid.add_argument("--report", default="test-results/hid-acceptance.json")
    wake = sub.add_parser("wake-acoustic", help="play a local phrase and verify real ESP-SR acoustic/UI feedback")
    wake.add_argument("--build-dir", default="firmware/p4_gesture/build_meoave")
    wake.add_argument("--port")
    wake.add_argument("--baud", default="921600")
    wake.add_argument("--flash", action="store_true", help="flash the selected product image first")
    wake.add_argument("--wav", help="local voice WAV; otherwise Windows SAPI generates Hi，喵喵")
    wake.add_argument("--negative-wav", help="non-keyword spoken PCM WAV for the negative control")
    wake.add_argument("--phrase", default="Hi，喵喵")
    wake.add_argument("--voice")
    wake.add_argument("--rate", type=int, default=0)
    wake.add_argument("--repeat", type=int, default=3)
    wake.add_argument("--gap", type=float, default=3.0)
    wake.add_argument("--listen-after", type=float, default=2.8)
    wake.add_argument("--timeout", type=float, default=60.0)
    wake.add_argument("--soak-seconds", type=int, default=420)
    wake.add_argument("--report", default="firmware/p4_gesture/test-results/wake-acoustic.json")
    wifi = sub.add_parser("wifi", help="one explicit C5 SDIO CMD5 status probe")
    wifi.add_argument("--report", default="test-results/wifi_sdio_probe.json")
    wifi.add_argument("--no-reset", action="store_true",
                      help="probe without requiring a preceding P4 program restart "
                           "(use after manually resetting the C5 into download boot); "
                           "never resets or flashes the C5 either way")
    c5 = sub.add_parser("c5", help="build the ESP32-C5 Hosted slave firmware and merge one image")
    c5.add_argument("--clean", action="store_true",
                    help="drop sdkconfig.c5 and reconfigure with cmake --fresh")
    c5.add_argument("--no-merge", action="store_true", help="skip the merged single-file image")
    c5_failure = c5.add_mutually_exclusive_group()
    c5_failure.add_argument("--fault-test", action="store_true", help="separate build_c5_fault: deliberately reset a PENDING candidate before Hosted; never provision this as baseline")
    c5_failure.add_argument("--hang-test", action="store_true", help="separate build_c5_hang: block a PENDING candidate before Hosted; requires external EN recovery, never provision as baseline")
    program = sub.add_parser("c5-program", help="manual-entry C5 SDIO identify/provision via P4 native USB")
    program.add_argument("--install", action="store_true", help="install the already-built P4 programmer profile")
    program.add_argument("--flash", action="store_true", help="write/verify the four C5 image regions after identification")
    program.add_argument("--reset-download", action="store_true", help="one explicit C5 reset with download straps held and radar inactive")
    program.add_argument("--facts", action="store_true", help="require C5 revision/boot register evidence")
    program.add_argument("--report", default="firmware/p4_gesture/test-results/c5_program.json")
    read = sub.add_parser("c5-read", help="read-only C5 Flash backup through P4 USB + SDIO")
    read.add_argument("--install", action="store_true", help="install P4 programmer only")
    read.add_argument("--limit", default="0x400000", help="read from zero, at most 4 MB")
    read.add_argument("--output", required=True, help="new backup path; never overwrite")
    read.add_argument("--report", required=True)
    minimal = sub.add_parser("c5-minimal", help="app-only C5 diagnosis with backup-based restore")
    minimal_action = minimal.add_mutually_exclusive_group(required=True)
    for name in ("prepare", "flash", "run", "observe", "build-restore", "restore"):
        minimal_action.add_argument("--"+name, action="store_true")
    minimal.add_argument("--straps-released", action="store_true")
    minimal.add_argument("--install", action="store_true")
    minimal.add_argument("--reset-download", action="store_true")
    for name in ("backup-report", "facts-report", "work-dir", "report"):
        minimal.add_argument("--"+name)
    diagnostic = sub.add_parser("c5-test", help="bounded Hosted SDIO speed/width matrix; no C5 flash")
    diagnostic.add_argument("--build", action="store_true")
    diagnostic.add_argument("--install", action="store_true")
    diagnostic.add_argument("--case", action="append", help="WIDTH:KHZ; repeat for a custom matrix")
    diagnostic.add_argument("--timeout", type=float, default=45)
    diagnostic.add_argument("--soak", type=float, default=3)
    diagnostic.add_argument("--poll-ms", type=int, choices=(0, 100), default=0)
    diagnostic.add_argument("--init-ms", type=int, default=4000)
    diagnostic.add_argument("--report", default="firmware/p4_gesture/test-results/c5_automated.json")
    font_parser = sub.add_parser("fonts", help="regenerate the LVGL Chinese font set")
    font_parser.add_argument("--set", choices=("round", "round_bold", "chill", "wenkai", "yozai", "maiyuan"),
                             help="switch font set: round=资源圆体(默认) round_bold=资源圆体粗 chill=寒蝉圆黑体 wenkai=霞鹜文楷 yozai=悠哉 maiyuan=荆南麦圆")
    font_parser.add_argument("--fetch", action="store_true",
                             help="download the fonts-v1 release bundle into build_ui_assets/fonts-src")
    font_parser.add_argument("--size", type=int, action="append", help="only this pixel size (repeatable)")
    font_parser.add_argument("--list", action="store_true", help="list the verified font sets")
    font_parser.add_argument("--check", action="store_true", help="only verify the source font hash")
    sub.add_parser("clean", help="remove only configured generated build directories")
    args = parser.parse_args()
    config = load_config()
    if args.command == "doctor":
        return doctor(config)
    if args.command == "build":
        build(config, args.profile, reconfigure=args.reconfigure)
    elif args.command == "test":
        tests(config)
    elif args.command == "all":
        build(config, "product", reconfigure=args.reconfigure)
        if args.hosted:
            build(config, "hosted", reconfigure=args.reconfigure)
        tests(config)
    elif args.command == "hardware":
        hardware(config, args)
    elif args.command == "hid":
        return hid_acceptance(config, args)
    elif args.command == "wake-acoustic":
        command = [python_command(config), str(TOOLS / "wake_acoustic_acceptance.py"),
                   "--build-dir", str(resolve(args.build_dir)),
                   "--baud", str(args.baud), "--phrase", args.phrase,
                   "--rate", str(args.rate), "--repeat", str(args.repeat),
                   "--gap", str(args.gap), "--listen-after", str(args.listen_after),
                   "--timeout", str(args.timeout), "--soak-seconds", str(args.soak_seconds),
                   "--report", str(resolve(args.report))]
        if args.port:
            command.extend(["--port", args.port])
        if args.flash:
            command.append("--flash")
        if args.wav:
            command.extend(["--wav", str(resolve(args.wav))])
        if args.negative_wav:
            command.extend(["--negative-wav", str(resolve(args.negative_wav))])
        if args.voice:
            command.extend(["--voice", args.voice])
        return subprocess.run(command, cwd=ROOT, env=environment(config)).returncode
    elif args.command == "wifi":
        return wifi_status(config, args)
    elif args.command == "c5":
        c5_build(config, args)
    elif args.command == "c5-program":
        command = [python_command(config), str(TOOLS / "c5_program.py"),
                   "--build-dir", str(project_dir(config) / cfg(config, "build", "programmer_dir", "build_meoave_programmer")),
                   "--payload", str(c5_dir(config) / cfg(config, "build", "c5_dir", "build_c5") / "c5_hosted_slave_merged.bin"),
                   "--report", str(resolve(args.report))]
        if args.install:
            command.append("--install")
        if args.flash:
            command.append("--flash")
        if args.reset_download:
            command.append("--reset-download")
        if args.facts:
            command.append("--facts")
        return subprocess.run(command, cwd=ROOT, env=environment(config)).returncode
    elif args.command == "c5-read":
        command = [python_command(config), str(TOOLS / "c5_read.py"),
                   "--limit", args.limit, "--output", str(resolve(args.output)),
                   "--report", str(resolve(args.report))]
        if args.install:
            command.append("--install")
        return subprocess.run(command, cwd=ROOT, env=environment(config)).returncode
    elif args.command == "c5-minimal":
        forwarded=[]
        for name in ("prepare", "flash", "run", "observe", "build-restore", "restore", "straps-released", "reset-download", "install"):
            if getattr(args,name.replace("-","_")): forwarded.append("--"+name)
        for name in ("backup-report", "facts-report", "work-dir", "report"):
            value=getattr(args,name.replace("-","_"))
            if value: forwarded.extend(["--"+name, str(resolve(value))])
        return subprocess.run([python_command(config), str(TOOLS / "c5_minimal.py"), *forwarded],
                              cwd=ROOT, env=environment(config)).returncode
    elif args.command == "c5-test":
        command = [python_command(config), str(TOOLS / "c5_test.py"),
                   "--timeout", str(args.timeout), "--soak", str(args.soak),
                   "--poll-ms", str(args.poll_ms), "--init-ms", str(args.init_ms),
                   "--report", str(resolve(args.report))]
        if args.build:
            command.append("--build")
        if args.install:
            command.append("--install")
        for case in args.case or []:
            command.extend(["--case", case])
        return subprocess.run(command, cwd=ROOT, env=environment(config)).returncode
    elif args.command == "fonts":
        fonts(config, args)
    elif args.command == "clean":
        clean(config)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
