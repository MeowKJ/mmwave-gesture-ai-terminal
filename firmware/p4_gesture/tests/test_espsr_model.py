"""Local asset/pack/tool tests; no acoustic detection or serial interaction."""
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from contextlib import redirect_stdout
import unittest
from types import SimpleNamespace
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[3] / "tools"
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

asset = load("espsr_model_test_subject", TOOLS / "espsr_model.py")
workflow = load("espsr_workflow_test_subject", TOOLS / "meoave.py")
NAME = "wn9_maomao"
META = "wakenet9_tts1h8_猫猫_3_0.636_0.641".encode()


def packed(name=NAME, metadata=META):
    files = {"_MODEL_INFO_": metadata, "wn9_index": b"testindex", "wn9_data": b"testdata"}
    header = struct.pack("<I32sI", 1, name.encode(), len(files))
    offset = 4 + 36 + len(files)*40
    payload = b""
    for filename, data in files.items():
        header += struct.pack("<32sII", filename.encode(), offset, len(data))
        offset += len(data)
        payload += data
    return header + payload


class EspSrModelTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.folder = self.root / NAME
        self.folder.mkdir()
        (self.folder / "_MODEL_INFO_").write_bytes(META)
        (self.folder / "wn9_data").write_bytes(b"coefficients")
        (self.folder / "wn9_index").write_bytes(b"index")
        self.image = self.root / "test.bin"

    def test_local_exact_model_and_fingerprint(self):
        first = asset.local_model(self.root, NAME)
        self.assertEqual(first["keyword"], "猫猫")
        self.assertLess(first["packed_bytes"], asset.PARTITION_BYTES)
        (self.folder / "wn9_data").write_bytes(b"new coefficients")
        self.assertNotEqual(first["sha256"], asset.local_model(self.root, NAME)["sha256"])

    def test_wrong_phrase_and_renaming_rejected(self):
        for word in ("Hi,喵喵", "你好猫猫", "猫猫;你好"):
            with self.subTest(word=word):
                (self.folder / "_MODEL_INFO_").write_text(f"wakenet9_test_{word}_3_0.6_0.6", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "not exactly"):
                    asset.local_model(self.root, NAME)

    def test_unrelated_assets_and_oversize_rejected(self):
        (self.folder / "wn9_data").write_bytes(b"x" * asset.PARTITION_BYTES)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            asset.local_model(self.root, NAME)
        (self.folder / "wn9_data").write_bytes(b"x")
        (self.root / "other").mkdir()
        with self.assertRaisesRegex(ValueError, "only"):
            asset.local_model(self.root, NAME)

    def test_empty_pack_is_not_ready(self):
        self.image.write_bytes(struct.pack("<I", 0))
        self.assertEqual(asset.inspect_image(self.image, NAME)["reason"], "MODEL_MISSING")

    def test_diagnostic_requires_explicit_preset_and_keeps_actual_phrase(self):
        self.image.write_bytes(packed("wn9_himiaomiao_tts", b"wakenet9_test_Hi,\xe5\x96\xb5\xe5\x96\xb5_3_0.6_0.6"))
        self.assertFalse(asset.inspect_image(self.image, "wn9_himiaomiao_tts")["ready"])
        self.assertTrue(asset.inspect_image(self.image, "wn9_himiaomiao_tts", "Hi,喵喵")["ready"])
        with tempfile.TemporaryDirectory() as td:
            project=Path(td)/"firmware/p4"
            project.mkdir(parents=True)
            sdk=project/"sdkconfig.product"
            sdk.write_text("CONFIG_DISPLAY_VERIFIED=y\n")
            fragment,_=asset.configure(project,project/"build",sdk,{"diagnostic_preset":"hi_miaomiao"})
            self.assertIn('CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD="Hi,喵喵"',fragment.read_text(encoding="utf-8"))
            self.assertIn("CONFIG_SR_WN_WN9_HIMIAOMIAO_TTS=y",sdk.read_text(encoding="utf-8"))
            asset.configure(project,project/"build",sdk,{"diagnostic_preset":""})
            self.assertIn('CONFIG_APP_MEOAVE_ESPSR_WAKE_WORD="猫猫"',sdk.read_text(encoding="utf-8"))
            self.assertIn("CONFIG_SR_WN_WN9_HIMIAOMIAO_TTS=n",sdk.read_text(encoding="utf-8"))
            with self.assertRaisesRegex(ValueError,"OR"):
                asset.configure(project,project/"build",sdk,{"diagnostic_preset":"hi_miaomiao","model_dir":str(self.root)})

    def test_unrelated_and_mislabeled_models_are_not_ready(self):
        self.image.write_bytes(packed("wn9_himiaomiao_tts"))
        self.assertEqual(asset.inspect_image(self.image, NAME)["reason"], "MODEL_MISSING")
        self.image.write_bytes(packed(metadata=b"wakenet9_test_Hi,miaomiao_3_0.6_0.6"))
        self.assertEqual(asset.inspect_image(self.image, NAME)["reason"], "MODEL_WORD_MISMATCH")

    def test_valid_pack_metadata_is_not_an_acoustic_test(self):
        self.image.write_bytes(packed())
        result = asset.inspect_image(self.image, NAME)
        self.assertTrue(result["ready"])
        self.assertEqual(result["keyword"], "猫猫")
        self.assertEqual(len(result["sha256"]), 64)

    def test_corrupt_tables_rejected(self):
        for data in (b"", b"\xff"*4, packed()[:45], packed()[:160]):
            self.image.write_bytes(data)
            with self.assertRaises(ValueError):
                asset.inspect_image(self.image, NAME)

    def test_local_configuration_scoped_and_asset_repack(self):
        with tempfile.TemporaryDirectory() as td:
            project = Path(td) / "firmware/p4"
            project.mkdir(parents=True)
            build = project / "build"
            sdk = project / "sdkconfig.product"
            original = "CONFIG_DISPLAY_VERIFIED=y\nCONFIG_AUDIO_48K=y\n"
            sdk.write_text(original)
            fragment, sha = asset.configure(project, build, sdk, {"model_dir": str(self.root)})
            self.assertTrue(fragment.is_file())
            self.assertIn(original, sdk.read_text(encoding="utf-8"))
            self.assertIn("CONFIG_ESP_SR_EXTERNAL_MODEL_PATH=", sdk.read_text(encoding="utf-8"))
            image = build / "srmodels/srmodels.bin"
            image.parent.mkdir()
            image.write_bytes(packed())
            before = sdk.stat().st_mtime_ns
            self.assertEqual(asset.configure(project, build, sdk, {"model_dir": str(self.root)})[1], sha)
            self.assertEqual(sdk.stat().st_mtime_ns, before)
            self.assertTrue(image.exists())
            # IDF canonicalizes the order and n -> '# ... is not set'. That
            # must not dirty sdkconfig and rebuild the whole dependency tree.
            canonical="\n".join(reversed(sdk.read_text(encoding="utf-8").splitlines())).replace(
                "CONFIG_SR_WN_WN9_HIMIAOMIAO_TTS=n", "# CONFIG_SR_WN_WN9_HIMIAOMIAO_TTS is not set")+"\n"
            sdk.write_text(canonical,encoding="utf-8")
            before=sdk.stat().st_mtime_ns
            asset.configure(project,build,sdk,{"model_dir":str(self.root)})
            self.assertEqual(sdk.stat().st_mtime_ns,before)
            (self.folder / "wn9_data").write_bytes(b"new data")
            self.assertNotEqual(asset.configure(project, build, sdk, {"model_dir": str(self.root)})[1], sha)
            self.assertFalse(image.exists())

    def test_feature_and_asset_change_invalidate_signature(self):
        p = self.root / "sdkconfig"
        args = ({}, "product", self.root, p, "defaults")
        old = workflow.build_signature(*args, {"MEOAVE_USE_ESP_SR": "0"})
        new = workflow.build_signature(*args, {"MEOAVE_USE_ESP_SR": "1"})
        self.assertNotEqual(old, new)
        changed = workflow.build_signature(*args, {"MEOAVE_USE_ESP_SR": "1", "MEOAVE_ESPSR_ASSET_SHA": "changed"})
        self.assertNotEqual(new, changed)

    def test_link_check_does_not_call_nonempty_pack_ready(self):
        build = self.root / "build"
        (build / "config").mkdir(parents=True)
        (build / "config/sdkconfig.json").write_text(json.dumps(
            {"APP_MEOAVE_ESPSR": True, "APP_MEOAVE_ESPSR_WAKE_MODEL": NAME}))
        (build / "CMakeCache.txt").write_text("CMAKE_NM:FILEPATH=tool/nm\n")
        (build / "test.elf").touch()
        (build / "srmodels").mkdir()
        (build / "srmodels/srmodels.bin").write_bytes(packed("wn9_other"))
        out = io.StringIO()
        with patch.dict(sys.modules, {"espsr_model": asset}), redirect_stdout(out), \
             patch.object(workflow.subprocess, "run", return_value=SimpleNamespace(stdout=
                 "0 T meoave_espsr_init\n0 T meo_wake_feed_audio\n0 T esp_wn_handle_from_name\n")):
            workflow.verify_product_espsr(build, {"MEOAVE_USE_ESP_SR": "1"})
        self.assertIn("MODEL_MISSING", out.getvalue())
        self.assertIn("disabled", out.getvalue())


if __name__ == "__main__":
    unittest.main()
