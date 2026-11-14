"""A successful compiler exit must not hide a discarded product UI."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "meoave_tools_link_test", Path(__file__).resolve().parents[3] / "tools" / "meoave.py")
workflow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workflow)


class ProductLinkTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        (self.build / "CMakeCache.txt").write_text("CMAKE_NM:FILEPATH=tool/nm.exe\n")
        (self.build / "product.elf").touch()

    def test_product_entry_present(self):
        with patch.object(workflow.subprocess, "run", return_value=SimpleNamespace(
                stdout="40000100 T meoave_app_create\n40000200 T meo_ui_create\n")) as run:
            workflow.verify_product_ui(self.build, {})
        self.assertEqual(run.call_args.args[0][0:2], ["tool/nm.exe", "--defined-only"])
        self.assertTrue(run.call_args.kwargs["check"])

    def test_discarded_renderer_rejected(self):
        with patch.object(workflow.subprocess, "run", return_value=SimpleNamespace(
                stdout="40000100 T meoave_app_create\n")):
            with self.assertRaisesRegex(RuntimeError, "meo_ui_create"):
                workflow.verify_product_ui(self.build, {})

    def test_discarded_entry_rejected(self):
        with patch.object(workflow.subprocess, "run", return_value=SimpleNamespace(
                stdout="40000200 T meo_ui_create\n")):
            with self.assertRaisesRegex(RuntimeError, "meoave_app_create"):
                workflow.verify_product_ui(self.build, {})

    def test_missing_nm_rejected(self):
        (self.build / "CMakeCache.txt").write_text("")
        with self.assertRaisesRegex(RuntimeError, "CMAKE_NM"):
            workflow.verify_product_ui(self.build, {})

    def test_ambiguous_elf_rejected(self):
        (self.build / "other.elf").touch()
        with self.assertRaisesRegex(RuntimeError, "one application ELF"):
            workflow.verify_product_ui(self.build, {})


class ProductBuildToolTest(unittest.TestCase):
    def setUp(self):
        wifi_verify = patch.object(workflow, "verify_product_wifi")
        wifi_verify.start()
        self.addCleanup(wifi_verify.stop)
        # Even a mocked build writes its configuration signature. Keep that
        # write away from the real firmware cache, or tests force a reconfigure.
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.project = {"p4_dir": temp.name}

    def test_bounded_jobs_and_rom_environment(self):
        with patch.object(workflow, "run") as run, \
                patch.object(workflow, "verify_product_ui") as verify, \
                patch.object(workflow, "environment", return_value={}), \
                patch.object(workflow, "esp_rom_elf_dir", return_value="tool/rom"):
            workflow.build({"project": self.project}, "product")
        self.assertEqual(run.call_args.args[0][-2:], ["-j", "8"])
        self.assertEqual(run.call_args.args[2]["ESP_ROM_ELF_DIR"], "tool/rom")
        verify.assert_called_once()

    def test_configured_jobs(self):
        with patch.object(workflow, "run") as run, \
                patch.object(workflow, "verify_product_ui"):
            workflow.build({"project": self.project, "build": {"jobs": 4}}, "product")
        self.assertEqual(run.call_args.args[0][-2:], ["-j", "4"])

    def test_invalid_jobs_rejected_before_configure(self):
        with patch.object(workflow, "run") as run:
            with self.assertRaisesRegex(ValueError, "build.jobs"):
                workflow.build({"build": {"jobs": 0}}, "product")
        run.assert_not_called()
    def test_incremental_skips_configure_when_signature_matches(self):
        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td) / "build_meoave"
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            (build_dir / "CMakeCache.txt").touch()
            cfg_dict = {"project": {"p4_dir": td}}
            env = workflow.environment(cfg_dict)
            if not env.get("ESP_ROM_ELF_DIR"):
                rom = workflow.esp_rom_elf_dir(cfg_dict)
                if rom:
                    env["ESP_ROM_ELF_DIR"] = rom
            sdkconfig = Path(td) / "sdkconfig.product"
            defaults = "sdkconfig.defaults;sdkconfig.product.defaults;sdkconfig.hosted.defaults"
            sig = workflow.build_signature(cfg_dict, "product", build_dir, sdkconfig, defaults, env)
            (build_dir / ".meoave_build_sig").write_text(sig, encoding="utf-8")
            with patch.object(workflow, "run") as run,                     patch.object(workflow, "verify_product_ui") as verify:
                workflow.build(cfg_dict, "product")
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[3], "build product")
            verify.assert_called_once()

    def test_reconfigure_flag_forces_configure(self):
        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td) / "build_meoave"
            build_dir.mkdir()
            (build_dir / "build.ninja").touch()
            (build_dir / "CMakeCache.txt").touch()
            cfg_dict = {"project": {"p4_dir": td}}
            env = workflow.environment(cfg_dict)
            if not env.get("ESP_ROM_ELF_DIR"):
                rom = workflow.esp_rom_elf_dir(cfg_dict)
                if rom:
                    env["ESP_ROM_ELF_DIR"] = rom
            sdkconfig = Path(td) / "sdkconfig.product"
            defaults = "sdkconfig.defaults;sdkconfig.product.defaults;sdkconfig.hosted.defaults"
            sig = workflow.build_signature(cfg_dict, "product", build_dir, sdkconfig, defaults, env)
            (build_dir / ".meoave_build_sig").write_text(sig, encoding="utf-8")
            with patch.object(workflow, "run") as run,                     patch.object(workflow, "verify_product_ui") as verify:
                workflow.build(cfg_dict, "product", reconfigure=True)
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args_list[0].args[3], "configure product")
            self.assertEqual(run.call_args_list[1].args[3], "build product")
            verify.assert_called_once()


class ProductWifiPolicyTest(unittest.TestCase):
    def test_real_generated_config_required(self):
        with tempfile.TemporaryDirectory() as td, patch.object(workflow,"verify_wifi_abi"):
            build = Path(td)
            path = build / "config/sdkconfig.json"
            path.parent.mkdir()
            values = {key: True for key in ("APP_MEOAVE_WIFI_HOSTED", "ESP_WIFI_REMOTE_ENABLED",
                "ESP_HOSTED_ENABLED", "ESP_HOSTED_CP_TARGET_ESP32C5", "ESP_HOSTED_SDIO_HOST_INTERFACE")}
            path.write_text(json.dumps(values))
            workflow.verify_product_wifi(build)
            for key in values:
                path.write_text(json.dumps(dict(values, **{key: False})))
                with self.subTest(key=key), self.assertRaises(RuntimeError):
                    workflow.verify_product_wifi(build)
            for key in ("APP_C5_SDIO_LOADER", "APP_C5_NATIVE_MAINTENANCE"):
                path.write_text(json.dumps(dict(values, **{key: True})))
                with self.subTest(key=key), self.assertRaises(RuntimeError):
                    workflow.verify_product_wifi(build)

    def test_legacy_migration_preserves_hardware_settings_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as td:
            sdk = Path(td)/"sdkconfig.product"
            defaults = Path(__file__).resolve().parents[1]/"sdkconfig.hosted.defaults"
            sdk.write_text("# CONFIG_APP_MEOAVE_WIFI_HOSTED is not set\nCONFIG_ESP_HOSTED_ENABLED=n\nCONFIG_PANEL_ROTATION=180\nCONFIG_AUDIO_GAIN=14\n")
            workflow.migrate_product_wifi(sdk, defaults)
            first = sdk.read_bytes()
            self.assertIn(b"CONFIG_APP_MEOAVE_WIFI_HOSTED=y", first)
            self.assertIn(b"CONFIG_ESP_HOSTED_ENABLED=y", first)
            self.assertIn(b"CONFIG_PANEL_ROTATION=180", first)
            self.assertIn(b"CONFIG_AUDIO_GAIN=14", first)
            workflow.migrate_product_wifi(sdk, defaults)
            self.assertEqual(first, sdk.read_bytes())

    def test_dwarf_layout_mismatch_rejected_even_when_sizes_agree(self):
        names=("bsp_wifi_hosted.c","rpc_rsp.c","rpc_wrap.c","esp_wifi_weak.c")
        layouts={name:{"wifi_ap_record_t":{"size":96,"members":[["he_ap",84,None,None]]}} for name in names}
        workflow.validate_wifi_abi(layouts)
        layouts["bsp_wifi_hosted.c"]["wifi_ap_record_t"]={"size":92,"members":[["he_ap",80,None,None]]}
        with self.assertRaisesRegex(RuntimeError,"ABI mismatch"):
            workflow.validate_wifi_abi(layouts)
        layouts["bsp_wifi_hosted.c"]["wifi_ap_record_t"]["size"]=96
        with self.assertRaisesRegex(RuntimeError,"ABI mismatch"):
            workflow.validate_wifi_abi(layouts)
        del layouts["bsp_wifi_hosted.c"]
        with self.assertRaisesRegex(RuntimeError,"missing DWARF"):
            workflow.validate_wifi_abi(layouts)
