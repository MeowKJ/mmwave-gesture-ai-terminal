import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import flash_usb as usb
import usb_iterate as iteration


def port(name="COM10", pid=0x4005, vid=0x303A):
    return SimpleNamespace(device=name, vid=vid, pid=pid, product="test")


class NativeUSBTests(unittest.TestCase):
    def test_exact_native_filter_no_uart_or_unknown_pid(self):
        with patch.object(usb.list_ports, "comports", return_value=[
                port("COM3", 0x8010, 0x1A86), port("COM5", 0x1001), port()]):
            self.assertEqual(usb.app_port().device, "COM10")
            with patch.object(usb.serial, "Serial") as serial:
                with self.assertRaises(RuntimeError):
                    usb.open_native("COM3", usb.APP_PIDS)
                serial.assert_not_called()

    def test_multiple_devices_are_refused(self):
        with patch.object(usb.list_ports, "comports", return_value=[port(), port("COM11")]):
            with self.assertRaises(RuntimeError):
                usb.app_port()

    def test_control_lines_set_before_open(self):
        fake = MagicMock()
        fake.dtr = fake.rts = False
        fake.open.side_effect = lambda: self.assertTrue(fake.dtr and fake.rts)
        with patch.object(usb, "select_port", return_value=port()), \
                patch.object(usb.serial, "Serial", return_value=fake) as serial:
            usb.open_native("COM10", usb.APP_PIDS)
            self.assertIsNone(serial.call_args.kwargs["port"])

    def test_flash_command_never_uses_stub_or_resets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = bytearray(320)
            image[0] = 0xE9
            struct.pack_into("<I", image, 32, 0xABCD5432)
            image[176:208] = bytes(range(32))
            (root / "app.bin").write_bytes(image)
            (root / "flasher_args.json").write_text(json.dumps({
                "flash_files": {"0x10000": "app.bin"},
                "app": {"file": "app.bin"}}), encoding="utf-8")
            for no_stub in (True, False):
                cmd = usb.build_flash_command(root, "COM9", no_stub)
                self.assertIn("--no-stub", cmd)
                self.assertEqual(cmd[cmd.index("--after") + 1], "no-reset")
                self.assertNotIn("hard-reset", cmd)
                self.assertNotIn("soft-reset", cmd)
            self.assertEqual(usb.image_identity(root)["elf_sha"], bytes(range(32)).hex())

    def test_query_rejects_stale_nonce_and_truncated_line(self):
        stream = MagicMock()
        stream.read.side_effect = [
            b"SYS:INFO token=old sha=" + b"a"*64 + b" reset=3 uptime_us=1 boot=12345678\n",
            b"SYS:INFO token=abcdef sha=" + b"a"*64 + b" reset=3 uptime_us=99 boot=1234",
            b"5678\n"]
        with patch.object(usb.secrets, "token_hex", return_value="abcdef"):
            reply = usb.query(stream, "INFO", usb.INFO_PATTERN, timeout=1)
        self.assertEqual(reply[3:], ("99", "12345678"))

    def test_boot_change_is_required_after_restart(self):
        info_old = ("token", "a"*64, "3", "5000000", "11111111")
        info_new = ("token", "a"*64, "3", "1000000", "22222222")
        power = ("token", "1", "1", "0", "1")
        with patch.object(usb, "app_port", return_value=port()), \
                patch.object(usb, "open_native") as opened, \
                patch.object(usb, "query", side_effect=[info_old, info_new, power]), \
                patch.object(usb.time, "sleep"):
            opened.return_value.__enter__.return_value = MagicMock()
            state = usb.check_application("a"*64, previous_boot="11111111")
        self.assertEqual(state["boot_id"], "22222222")

    def test_pressed_key_does_not_count_as_unattended_pass(self):
        with patch.object(usb, "app_port", return_value=port()), \
                patch.object(usb, "open_native"), \
                patch.object(usb, "query", side_effect=[
                    ("token", "a"*64, "3", "1", "11111111"),
                    ("token", "1", "1", "1", "1")]):
            with self.assertRaisesRegex(RuntimeError, "SW1"):
                usb.check_application("a"*64)

    def test_failed_restart_retains_before_and_after(self):
        before = {"port": "COM10", "boot_id": "11111111"}
        after = {"port": "COM10", "boot_id": "22222222", "reset_reason": 4,
                 "marker_retained": True}
        report = {}
        with patch.object(usb, "check_application", side_effect=[before, after]), \
                patch.object(usb, "open_native"), \
                patch.object(usb, "read_fault_diagnostic", return_value={"valid": "1"}):
            with self.assertRaisesRegex(RuntimeError, "reset reason"):
                usb.restart_once("a"*64, report=report)
        self.assertFalse(report["ok"])
        self.assertEqual(report["after"], after)
        self.assertEqual(report["before"], before)
        self.assertEqual(report["fault"], {"valid": "1"})

    def test_runtime_requires_real_progress_without_visual_commands(self):
        stream = MagicMock()
        stream.read.side_effect = [
            b"FLS2:STATUS profile=official_widgets ready=1 usb=1 rgb888=1\n"
            b"FLS2:TOUCH init=1 addr=0x15 id=0xB7 id_ok=1 id_err=0\n"
            b"FLS2:TOUCH_IO driver_ok=10 driver_err=0 probe_ok=0\n"
            b"FLS2:PONG uptime_us=1000000\n",
            b"FLS2:TOUCH_IO driver_ok=20 driver_err=0 probe_ok=0\n"
            b"FLS2:PONG uptime_us=2000000\n"]
        with patch.object(usb, "open_native") as opened:
            opened.return_value.__enter__.return_value = stream
            result = usb.check_fls2_runtime("COM10")
        self.assertGreater(result["driver_ok_last"], result["driver_ok_first"])
        self.assertEqual(result["visual"], "NOT_RETESTED")
        for call in stream.write.call_args_list:
            self.assertEqual(call.args, (b"PING\nSTATUS\nTOUCH ID\nTOUCH\n",))

    def test_runtime_fails_actual_driver_errors(self):
        stream = MagicMock()
        stream.read.return_value = b"FLS2:TOUCH_IO driver_ok=10 driver_err=1 probe_ok=0\n"
        with patch.object(usb, "open_native") as opened:
            opened.return_value.__enter__.return_value = stream
            report = {}
            with self.assertRaisesRegex(RuntimeError, "communication errors"):
                usb.check_fls2_runtime("COM10", report=report)
            self.assertFalse(report["ok"])
            self.assertTrue(report["lines"])

    def test_panic_reset_is_never_a_clean_pass(self):
        report = {}
        with patch.object(usb, "read_fault_diagnostic", return_value={"valid": "1"}):
            with self.assertRaisesRegex(RuntimeError, "reset reason"):
                usb.require_clean_reset({"reset_reason": 4, "marker_retained": True,
                                         "port": "COM10"}, report)
        self.assertEqual(report["fault"]["valid"], "1")

    def test_unexpected_reboot_during_runtime_is_rejected(self):
        cycle = {}
        application = {"port": "COM10", "boot_id": "11111111"}
        after = {"port": "COM10", "boot_id": "22222222", "reset_reason": 3,
                 "marker_retained": True}
        with patch.object(usb, "check_fls2_runtime"), \
                patch.object(usb, "check_application", return_value=after):
            with self.assertRaisesRegex(RuntimeError, "Unexpected additional reboot"):
                iteration.verify_runtime(cycle, application, "a"*64, 30)
        self.assertFalse(cycle["ok"])
        self.assertEqual(cycle["application_after_runtime"], after)

    def test_runtime_does_not_accept_cached_zero_id(self):
        stream = MagicMock()
        stream.read.side_effect = [
            b"FLS2:STATUS profile=official_widgets ready=1 usb=1 rgb888=1\n"
            b"FLS2:TOUCH init=1 addr=0x15 id=0x00 id_ok=1 id_err=0\n"
            b"FLS2:TOUCH_IO driver_ok=10 driver_err=0 probe_ok=0\n"
            b"FLS2:PONG uptime_us=1000000\n",
            b"FLS2:TOUCH init=1 addr=0x15 id=0xB7 id_ok=2 id_err=0\n"
            b"FLS2:TOUCH_IO driver_ok=20 driver_err=0 probe_ok=1\n"
            b"FLS2:PONG uptime_us=2000000\n"]
        with patch.object(usb, "open_native") as opened:
            opened.return_value.__enter__.return_value = stream
            result = usb.check_fls2_runtime("COM10")
        self.assertEqual(stream.read.call_count, 2)
        self.assertEqual(result["touch_id"], "0xB7")

    def test_reset_failure_never_triggers_reflash(self):
        report = {}
        with patch.object(usb, "image_identity", return_value={"elf_sha": "a"*64, "image_count": 3}), \
                patch.object(usb, "build_flash_command", return_value=["esptool"]), \
                patch.object(usb, "espressif_ports", return_value=[port("COM9", 0x12)]), \
                patch.object(usb, "select_port", return_value=port("COM9", 0x12)), \
                patch.object(usb, "rom_port", return_value=port("COM9", 0x12)), \
                patch.object(usb.time, "sleep"), \
                patch.object(usb.subprocess, "run", return_value=SimpleNamespace(
                    returncode=0, stdout="Hash of data verified\n"*3, stderr="")) as writer, \
                patch.object(usb, "cpu_reset_from_rom", side_effect=RuntimeError("reset failed")):
            with self.assertRaisesRegex(RuntimeError, "reset failed"):
                usb.flash_once(Path("unused"), report=report)
        self.assertEqual(writer.call_count, 1)
        self.assertTrue(report["flash_verified"])
        self.assertEqual(report["stage"], "program_reset")

    def test_incomplete_verification_does_not_reset(self):
        with patch.object(usb, "image_identity", return_value={"elf_sha": "a"*64, "image_count": 3}), \
                patch.object(usb, "build_flash_command", return_value=["esptool"]), \
                patch.object(usb, "espressif_ports", return_value=[port("COM9", 0x12)]), \
                patch.object(usb, "select_port", return_value=port("COM9", 0x12)), \
                patch.object(usb.time, "sleep"), \
                patch.object(usb.subprocess, "run", return_value=SimpleNamespace(
                    returncode=0, stdout="Hash of data verified\n"*2, stderr="")), \
                patch.object(usb, "cpu_reset_from_rom") as reset:
            with self.assertRaisesRegex(RuntimeError, "verification failed"):
                usb.flash_once(Path("unused"))
            reset.assert_not_called()


class CPUResetTests(unittest.TestCase):
    def run_reset(self, hold=True, revision=302, lose_reply=False):
        registers = {0x501101C4: (1 << 29) if hold else 0,
                     0x500E0004: 1 << 29, 0x500E0020: 1 << 29,
                     0x50110008: 0x1234, 0x50111014: 0x80028002}
        esp = MagicMock()
        esp.get_chip_revision.return_value = revision
        esp.sync_stub_detected = False
        esp.read_reg.side_effect = lambda addr: registers[addr]

        def write(addr, value):
            registers[addr] = value
            if lose_reply and addr == 0x50111014:
                raise usb.serial.SerialException("expected USB disconnect")

        esp.write_reg.side_effect = write
        with patch.object(usb, "open_native"), \
                patch("esptool.targets.esp32p4.ESP32P4ROM", return_value=esp):
            result = usb.cpu_reset_from_rom("COM9")
        return result, esp, registers

    def test_cpu_reset_preserves_all_other_register_bits(self):
        result, esp, registers = self.run_reset(lose_reply=True)
        self.assertEqual(registers[0x50110008], 0x1230)
        self.assertEqual(registers[0x50111014], 0x80028002 | (1 << 13) | (1 << 29))
        self.assertEqual(result["kind"], "HP_CPU_ONLY")
        self.assertIn("SerialException", result["transport_note"])
        self.assertNotIn("ok", result)  # Transport loss alone is never PASS.
        self.assertTrue(all(call.args[0] in (0x50110008, 0x50111014)
                            for call in esp.write_reg.call_args_list))

    def test_missing_hold_refuses_reset(self):
        with self.assertRaisesRegex(RuntimeError, "GPIO29"):
            self.run_reset(hold=False)

    def test_unsupported_silicon_refuses_register_writes(self):
        with self.assertRaisesRegex(RuntimeError, "rev3"):
            self.run_reset(revision=101)


if __name__ == "__main__":
    unittest.main()
