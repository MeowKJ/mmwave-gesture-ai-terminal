"""New development-board UART tools must never flash C5 or erase the chip."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import p4_bringup as tool


class BringupTests(unittest.TestCase):
    def test_uart_rate_follows_selected_build_not_rom_baud(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build/'config').mkdir()
            config = build/'config/sdkconfig.json'
            config.write_text(json.dumps({'ESP_CONSOLE_UART_BAUDRATE':460800}), encoding='utf-8')
            self.assertEqual(tool.meoave.console_uart_baud(build), 460800)
            config.write_text(json.dumps({'ESP_CONSOLE_UART_BAUDRATE':12345}), encoding='utf-8')
            with self.assertRaises(ValueError): tool.meoave.console_uart_baud(build)

    def args_file(self, directory, chip="esp32p4", extra=None):
        info={"extra_esptool_args":{"chip":chip}, "flash_files":{
            "0x2000":"bootloader/bootloader.bin", "0x8000":"partition_table/partition-table.bin",
            "0x10000":"p4_c5_bringup.bin"},
            "write_flash_args":["--flash-mode","dio","--flash-size","16MB","--flash-freq","40m"]}
        if extra: info["flash_files"].update(extra)
        (Path(directory)/"flasher_args.json").write_text(json.dumps(info),encoding="utf-8")

    def test_rom_uart_strict_p4_no_chip_erase_or_force(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(tool,"BUILD",Path(tmp)), patch.object(tool,"run") as run:
            self.args_file(tmp)
            report={}
            tool.flash({}, {}, "COM99", report)
            cmd=run.call_args.args[0]
            self.assertEqual(cmd[cmd.index("--chip")+1],"esp32p4")
            self.assertEqual(cmd[cmd.index("--port")+1],"COM99")
            self.assertIn("--no-stub",cmd)
            self.assertNotIn("--force",cmd)
            self.assertNotIn("erase-flash",cmd)
            self.assertNotIn("esp32c5",cmd)
            self.assertTrue(report["p4_flash_command_completed"])

    def test_reject_c5_image_before_opening_uart(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(tool,"BUILD",Path(tmp)), patch.object(tool,"run") as run:
            self.args_file(tmp,"esp32c5")
            with self.assertRaisesRegex(RuntimeError,"Not a P4"):
                tool.flash({}, {}, "COM99", {})
            run.assert_not_called()

    def test_reject_unexpected_region(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(tool,"BUILD",Path(tmp)), patch.object(tool,"run") as run:
            self.args_file(tmp,extra={"0x9000":"nvs.bin"})
            with self.assertRaisesRegex(RuntimeError,"Unexpected"):
                tool.flash({}, {}, "COM99", {})
            run.assert_not_called()

    def test_explicit_port_required(self):
        for args in (["--flash"],["--capture","5"]):
            with self.subTest(args=args), self.assertRaises(SystemExit) as exc:
                tool.main(args)
            self.assertEqual(exc.exception.code,2)

    def test_no_commands_without_capture(self):
        with self.assertRaises(SystemExit) as exc:
            tool.main(["--build","--command","C5RESET"])
        self.assertEqual(exc.exception.code,2)

    def test_profiles_reject_competing_sdio_owner_commands(self):
        for profile, command in (("hosted","PROBE"),("hosted","C5RESET"),("raw","CONNECT"),("raw","VERIFY")):
            with self.subTest(profile=profile, command=command), self.assertRaises(SystemExit):
                tool.main(["--profile",profile,"--port","COM99","--capture","1","--command",command])

    def test_cached_ready_does_not_pass_live_rpc_acceptance(self):
        with self.assertRaises(RuntimeError):
            tool.hosted_result("VERIFY", "MEO:HOSTED_STATUS state=READY source=CACHED\n")

    def test_rejected_command_cannot_pass_with_old_result(self):
        with self.assertRaises(RuntimeError):
            tool.hosted_result("VERIFY", "MEO:HOSTED_ACCEPT command=VERIFY accepted=0\n"
                               "MEO:HOSTED_VERIFY run=1 passed=1 samples=10 error=0x0\n")

    def test_failure_or_missing_terminal_is_not_success(self):
        for terminal in ("", "MEO:HOSTED_CONNECT passed=0 error=0x107\n",
                         "MEO:HOSTED_CONNECT passed=10 error=0x0\n",
                         "MEO:HOSTED_CONNECT passed=1 error=0x0"):
            with self.subTest(terminal=terminal), self.assertRaises(RuntimeError):
                tool.hosted_result("CONNECT", "MEO:HOSTED_ACCEPT command=CONNECT accepted=1\n" + terminal)

    def test_connect_and_verify_terminal_formats(self):
        for command, terminal in (("CONNECT", "MEO:HOSTED_CONNECT passed=1 error=0x0\r\n"),
                                  ("VERIFY", "MEO:HOSTED_VERIFY run=2 passed=1 samples=10 error=0x0\n")):
            tool.hosted_result(command, f"MEO:HOSTED_ACCEPT command={command} accepted=1\n" + terminal)

    def test_reject_stale_raw_image_for_hosted_before_uart(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(tool,"BUILD",Path(tmp)), patch.object(tool,"run") as run:
            self.args_file(tmp)
            (Path(tmp)/"p4_c5_bringup.bin").write_bytes(b"raw baseline only")
            with self.assertRaisesRegex(RuntimeError,"Hosted profile missing"):
                tool.flash({}, {}, "COM99", {"profile":"hosted"})
            run.assert_not_called()

    def test_hosted_source_cannot_run_raw_probe_or_flash_write(self):
        source=(tool.PROJECT/"main/main_hosted.c").read_text(encoding="utf-8")
        self.assertNotIn("sdmmc_card_init(",source)
        self.assertNotIn("gpio_set_level(",source)
        self.assertNotIn("esp_hosted_slave_ota_",source)
        self.assertNotIn("esp_hosted_init();",source)
