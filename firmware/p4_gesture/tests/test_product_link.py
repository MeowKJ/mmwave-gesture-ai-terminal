"""A successful compiler exit must not hide a discarded product UI."""
import importlib.util
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
    def test_bounded_jobs_and_rom_environment(self):
        with patch.object(workflow, "run") as run, \
                patch.object(workflow, "verify_product_ui") as verify, \
                patch.object(workflow, "environment", return_value={}), \
                patch.object(workflow, "esp_rom_elf_dir", return_value="tool/rom"):
            workflow.build({}, "product")
        self.assertEqual(run.call_args.args[0][-2:], ["-j", "8"])
        self.assertEqual(run.call_args.args[2]["ESP_ROM_ELF_DIR"], "tool/rom")
        verify.assert_called_once()

    def test_configured_jobs(self):
        with patch.object(workflow, "run") as run, \
                patch.object(workflow, "verify_product_ui"):
            workflow.build({"build": {"jobs": 4}}, "product")
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
            defaults = "sdkconfig.defaults;sdkconfig.product.defaults"
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
            defaults = "sdkconfig.defaults;sdkconfig.product.defaults"
            sig = workflow.build_signature(cfg_dict, "product", build_dir, sdkconfig, defaults, env)
            (build_dir / ".meoave_build_sig").write_text(sig, encoding="utf-8")
            with patch.object(workflow, "run") as run,                     patch.object(workflow, "verify_product_ui") as verify:
                workflow.build(cfg_dict, "product", reconfigure=True)
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args_list[0].args[3], "configure product")
            self.assertEqual(run.call_args_list[1].args[3], "build product")
            verify.assert_called_once()
