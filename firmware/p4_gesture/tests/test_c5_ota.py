"""Execute the actual C5 OTA RPC handlers against fault-injected IDF shims.

No device is opened. This checks state/error handling, not SDIO or flash hardware.
Set MEOAVE_HOST_CC to a native gcc/clang executable when it is not on PATH.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "firmware/c5_hosted/main/slave_control.c"
FIXTURE = Path(__file__).with_name("fixtures") / "c5_ota_harness.c"
CASES = (
    "happy", "activate_idle", "write_idle", "end_idle", "begin_twice",
    "begin_missing_slot", "begin_sdk_failure", "begin_running_slot",
    "begin_pending_boot", "begin_missing_running", "begin_missing_boot",
    "first_short", "first_invalid", "write_failure", "write_null_payload",
    "write_null_data", "write_empty", "write_overflow", "end_empty",
    "end_invalid_image", "repeat_end", "activate_in_progress",
    "activate_failed", "timer_oom", "boot_select_failure", "timer_failure",
    "restore_failure", "repeat_activate", "begin_after_activate",
    "write_after_end", "restart_callback", "null_request", "response_oom",
    "new_begin_after_failure", "begin_after_completed", "restore_confirm_failure",
)


class C5OtaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("MEOAVE_HOST_CC") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            raise unittest.SkipTest("native C compiler required (MEOAVE_HOST_CC or gcc/clang)")
        source = SOURCE.read_text(encoding="utf-8")
        enum = source[source.index("enum {\n\tOTA_NOT_STARTED"):]
        enum = enum[:enum.index("};") + 2]
        state = source[source.index("static esp_ota_handle_t handle;"):]
        state = state[:state.index("extern esp_err_t wlan_sta_rx_callback")]
        handlers = source[source.index("/* OTA session operations"):]
        handlers = handlers[:handlers.index("static void heartbeat_timer_cb")]
        fixture = FIXTURE.read_text(encoding="utf-8")
        cls.temp = tempfile.TemporaryDirectory(prefix="meoave-ota-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executables = []
        for validation, rollback in ((0,0), (1,0), (0,1), (1,1)):
            path = Path(cls.temp.name) / f"ota_{validation}_rollback_{rollback}.c"
            path.write_text(fixture.replace("/* ACTUAL_OTA_CODE */", enum +
                "\nstatic uint8_t ota_status = OTA_NOT_STARTED;\n" + state + handlers), encoding="utf-8")
            exe = path.with_suffix(".exe" if os.name == "nt" else "")
            result = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", f"-DH_OTA_CHECK_IMAGE_VALIDITY={validation}",
                *(["-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1"] if rollback else []),
                str(path), "-o", str(exe)], capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables.append(exe)

    def test_rpc_fault_matrix(self):
        for exe in self.executables:
            for case in CASES:
                with self.subTest(validation=exe.stem, case=case):
                    result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
