#!/usr/bin/env python3
"""Prepare an isolated, auditable FLS2 official-display build. Does not flash.

The downloaded OSPTEK source is never edited. Generated third-party sources,
configuration, unified diffs and the audit report stay in build_fls2_source/.
"""
import difflib
import argparse
import hashlib
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
OFFICIAL = (HERE.parent / "official_downloads" /
            "display-amoled-2inch-460x460-a0fbbd6b1ddfbd1eaa7cd881b999e46b47d1d4ff" /
            "examples" / "esp32p4-idf6-co5300-mipi-lvgl-port-lvgl9")
OUT = HERE / "build_fls2_source"


def config_items(text):
    result = {}
    for line in text.splitlines():
        match = re.match(r"(CONFIG_\w+)=(.*)$", line)
        unset = re.match(r"# (CONFIG_\w+) is not set$", line)
        if match:
            result[match[1]] = match[2]
        elif unset:
            result[unset[1]] = "n"
    return result


def source_text(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def verify():
    """Check the configured build without regenerating its sources/config."""
    official = config_items(source_text(OFFICIAL / "sdkconfig"))
    actual = config_items(source_text(OUT / "sdkconfig"))
    lv_diff = {key: [official.get(key), actual.get(key)]
               for key in official.keys() | actual.keys()
               if key.startswith("CONFIG_LV_") and official.get(key) != actual.get(key)}
    assert not lv_diff, f"LVGL configuration drift: {lv_diff}"
    assert actual.get("CONFIG_APP_FLS2_OFFICIAL_BASELINE") == "y"
    assert actual.get("CONFIG_LV_COLOR_DEPTH") == "24"
    assert source_text(OUT / "co5300_init_cmds.h") == source_text(OFFICIAL / "main/co5300_init_cmds.h")
    audit = json.loads(source_text(OUT / "audit.json"))
    expected_main = source_text(OFFICIAL / "main/main.c")
    for old, new in audit["source_changes"].items():
        assert expected_main.count(old) == 1
        expected_main = expected_main.replace(old, new)
    assert source_text(OUT / "main.c") == expected_main, "Unexpected display-source change"
    for official_file in (OFFICIAL / "components/esp_lcd_touch_cst820").rglob("*"):
        if official_file.is_file():
            relative = official_file.relative_to(OFFICIAL)
            assert (HERE / relative).read_bytes() == official_file.read_bytes(), relative
    commands = json.loads(source_text(HERE / "build_fls2_official/compile_commands.json"))
    main_sources = {Path(entry["file"]).resolve() for entry in commands
                    if "__idf_main.dir" in entry["command"]}
    expected_sources = {OUT / "main.c"} | {
        HERE / "main" / name for name in
        ("fls2_official_board.c", "fls2_touch_diag.c", "fls2_display_diag.c", "fls2_test_loop.c",
         "bsp_common.c", "bsp_power.c", "bsp_usb.c")}
    assert main_sources == expected_sources, f"Unexpected main sources: {main_sources}"
    driver = source_text(HERE / "managed_components/espressif__esp_lcd_co5300/esp_lcd_co5300_mipi.c")
    assert "LCD ID read skipped by local test patch" in driver, "Re-audit the ID-read patch/report"
    audit["post_configure_checks"] = {
        "lvgl_config_matches_official": True,
        "command_header_matches_official": True,
        "touch_component_matches_official": True,
        "only_expected_main_sources": sorted(str(path) for path in main_sources),
        "touch_diagnostics": "Link wrappers observe original ID/data reads; TOUCH ON adds "
                             "serialized 0x00/15-byte and 0xA7/1-byte reads on the same IO. "
                             "Probe and official-driver read counters are separate. "
                             "No scans, writes, reset changes or display changes.",
        "autotest_at_boot": actual.get("CONFIG_APP_FLS2_AUTOTEST") == "y",
        "display_diagnostics": "Optional CONFIG_APP_FLS2_AUTOTEST enables an on-device repeat "
                               "schedule: widgets 8s, colors 14s slot, hardware bars 22s slot. "
                               "DCS probe is excluded from the loop. LOOP OFF/ON controls it; "
                               "manual visual/probe commands pause it. DISPLAY BARS enables "
                               "DSI host hardware bars for 20s; DISPLAY NORMAL restores the "
                               "framebuffer pipeline. No clock/timing or callback changes. "
                               "Draw success is host-side only, not panel reception proof. "
                               "DISPLAY COLORS uses a temporary LVGL screen; DISPLAY HW reads "
                               "host/GPIO registers, not physical voltage. DISPLAY PROBE is an "
                               "experimental target-specific bounded DCS reader based on IDF 6.1 "
                               "HAL packet ordering; it pauses/restores video configuration and "
                               "does not alter the official initialization source or libraries. "
                               "Probe timeout is not a broken-panel verdict. A busy read engine "
                               "requires reboot; automatic loop resume is rejected in that state.",
        "remaining_config_differences": {
            key: {"official": official.get(key), "baseline": actual.get(key)}
            for key in sorted(official.keys() | actual.keys())
            if official.get(key) != actual.get(key)},
    }
    (OUT / "audit.json").write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: official LVGL config, command header, CST820 component, four source substitutions")
    print("PASS: compile database excludes custom display/touch/UI/audio/radar sources")
    print("CAUTION: existing CO5300 ID-read skip remains; panel ACK is not verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", help="audit after CMake, without regenerating")
    mode.add_argument("--enable-loop", action="store_true",
                      help="enable boot autotest in an existing isolated config; preserve sources/audit history")
    args = parser.parse_args()
    if args.verify:
        verify()
        return
    if args.enable_loop:
        path = OUT / "sdkconfig"
        text = source_text(path)
        assert config_items(text).get("CONFIG_APP_FLS2_OFFICIAL_BASELINE") == "y"
        lines = [line for line in text.splitlines()
                 if not re.match(r"(?:# )?CONFIG_APP_FLS2_AUTOTEST\b", line)]
        path.write_text("\n".join(lines + ["CONFIG_APP_FLS2_AUTOTEST=y", ""]), encoding="utf-8")
        audit_path = OUT / "audit.json"
        audit = json.loads(source_text(audit_path))
        audit["autotest_configuration"] = {
            "enabled_at_boot": True,
            "schedule": "widgets 8s -> colors 14s slot -> hardware bars 22s slot -> repeat",
            "dcs_probe": "excluded; once-per-boot manual only; busy engine blocks resume",
            "visual_verdict": "never inferred automatically",
        }
        audit_path.write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Enabled persistent boot loop in isolated config; historical audit/source/config preserved otherwise")
        return
    OUT.mkdir(exist_ok=True)
    original = source_text(OFFICIAL / "main/main.c")
    adapted = original
    replacements = {
        "#define EXAMPLE_PIN_NUM_LCD_RST  -1": "#define EXAMPLE_PIN_NUM_LCD_RST  GPIO_NUM_12",
        "#define EXAMPLE_TOUCH_I2C_SCL       (GPIO_NUM_8)": "#define EXAMPLE_TOUCH_I2C_SCL       (GPIO_NUM_9)",
        "#define EXAMPLE_TOUCH_I2C_SDA       (GPIO_NUM_7)": "#define EXAMPLE_TOUCH_I2C_SDA       (GPIO_NUM_8)",
        "void app_main(void)": "void fls2_official_demo(void)",
    }
    for old, new in replacements.items():
        assert adapted.count(old) == 1, f"Official source changed: {old}"
        adapted = adapted.replace(old, new)
    (OUT / "main.c").write_text(adapted, encoding="utf-8")
    official_cmds = source_text(OFFICIAL / "main/co5300_init_cmds.h")
    (OUT / "co5300_init_cmds.h").write_text(official_cmds, encoding="utf-8")
    (OUT / "main.diff").write_text("".join(difflib.unified_diff(
        original.splitlines(True), adapted.splitlines(True),
        fromfile="official/main.c", tofile="custom_board/main.c")), encoding="utf-8")

    official_config = source_text(OFFICIAL / "sdkconfig")
    local_config = source_text(HERE / "sdkconfig")
    # Keep the proven board/flash/USB settings; copy EVERY LVGL setting from
    # official sdkconfig, not merely the defaults (which omit RGB888).
    local_lines = [line for line in local_config.splitlines()
                   if not re.match(r"(?:# )?CONFIG_LV_", line)
                   and not re.match(r"(?:# )?CONFIG_APP_FLS2_OFFICIAL_BASELINE\b", line)]
    official_lv_lines = [line for line in official_config.splitlines()
                         if re.match(r"(?:# )?CONFIG_LV_", line)]
    config_path = OUT / "sdkconfig"
    config_path.write_text("\n".join(local_lines + official_lv_lines +
                           ["CONFIG_APP_FLS2_OFFICIAL_BASELINE=y", ""]), encoding="utf-8")

    import yaml  # Run with the installed IDF Python environment.
    a = yaml.safe_load(source_text(OFFICIAL / "dependencies.lock"))["dependencies"]
    b = yaml.safe_load(source_text(HERE / "dependencies.lock"))["dependencies"]
    deps = {}
    for key, entry in a.items():
        actual = b[key]
        assert entry["version"] == actual["version"], key
        assert entry.get("component_hash") == actual.get("component_hash"), key
        deps[key] = {"version": entry["version"], "locked_hash_matches": True}

    ca, cb = config_items(official_config), config_items(local_config)
    diff = {key: {"official": ca.get(key), "application": cb.get(key)}
            for key in sorted(ca.keys() | cb.keys()) if ca.get(key) != cb.get(key)}
    report = {
        "official_root": str(OFFICIAL),
        "generated_main": str(OUT / "main.c"),
        "source_changes": replacements,
        "commands_sha256": hashlib.sha256(official_cmds.encode()).hexdigest(),
        "dependencies": deps,
        "config_differences_before_baseline": diff,
        "driver_caveat": "Locked package hash/version match does NOT prove unchanged source. "
                         "Existing managed CO5300 MIPI source skips the DCS ID read. "
                         "This baseline retains and prominently reports that diagnostic patch. "
                         "Historical hang is reported by previous work but has not been "
                         "reproduced by this audit; no panel ACK or hardware-fault claim.",
        "excluded": ["gesture UI", "audio", "radar", "I2C scan", "automatic startup BAR -> NONE", "automatic startup color selftest"],
        "telemetry": "VSYNC deliberately NOT_MEASURED: official LVGL callback registration "
                     "replaces previously registered callbacks. Ready means UI created only.",
    }
    (OUT / "audit.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Prepared: {OUT}")
    print(f"Config: {config_path}")
    print("Official main diff: four substitutions only; official header copied exactly.")
    print("All official locked dependency versions/hashes match; ID-read source patch remains.")


if __name__ == "__main__":
    main()
