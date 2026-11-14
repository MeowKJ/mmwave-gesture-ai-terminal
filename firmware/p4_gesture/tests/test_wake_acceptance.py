"""Monitor-contract tests; simulated USB is never an acoustic test."""
import importlib.util
from pathlib import Path
import sys
import unittest
from contextlib import nullcontext
from types import SimpleNamespace
from unittest.mock import patch

TOOLS=Path(__file__).resolve().parents[3]/"tools"
with patch.object(sys,"path",[str(TOOLS),*sys.path]):
    spec=importlib.util.spec_from_file_location("wake_acceptance_subject",TOOLS/"wake_acceptance.py")
    monitor=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(monitor)


class WakeAcceptanceTest(unittest.TestCase):
    def run_monitor(self, *, missing=False, progress=True, acoustic=0, text=0, minimum=0,
                    armed=True, guarded=False, readiness_lost=False, reboot=False, fault=False,
                    rebuilding=False, stuck_rebuilding=False):
        class Link:
            def __init__(self,stream,report):
                self.calls=0
            def page(self,page):
                assert page=="eyes"
            def request(self,command,prefix):
                assert command=="UI" and prefix=="MEO:WAKE_STATUS"
                self.calls+=1
                if (rebuilding and self.calls==2) or (stuck_rebuilding and self.calls>1):
                    return dict(linked="1",loaded="0",keyword_ready="0",armed="0",requested="1",
                                guarded="0",error="MODEL_REBUILDING",chunks=str(self.calls),acoustic="0",text_tests="0")
                return dict(linked="1",loaded="0" if missing or (readiness_lost and self.calls>1) else "1",keyword_ready="1",
                            armed="1" if armed else "0", requested="1", guarded="1" if guarded else "0",
                            error="NONE",chunks=str(self.calls if progress else 1),
                            acoustic=str(acoustic if self.calls>1 else 0),
                            text_tests=str(text if self.calls>1 else 0))
        boots=iter(["same","different" if reboot else "same"])
        usb=SimpleNamespace(
            image_identity=lambda build:{"elf_sha":"sha"},
            check_application=lambda sha,timeout:{"port":"native","boot_id":next(boots)},
            open_native=lambda port,pids:nullcontext(object()),APP_PIDS={0x4005},
            read_fault_diagnostic=lambda port:{"valid":"1" if fault else "0"})
        report={"transcript":[],"acoustic_acceptance":"PENDING_EXTERNAL_VOICE"}
        with patch.object(monitor,"Link",Link),patch.object(monitor,"usb",usb),\
             patch.object(monitor.time,"monotonic",side_effect=[0,0,0.5,1,4] if rebuilding else
                          [0,0,0.5,4] if stuck_rebuilding else [0,0,4]),patch.object(monitor.time,"sleep"):
            monitor.run(Path("."),3,minimum,report)
        return report

    def test_progress_without_voice_stays_unaccepted(self):
        report=self.run_monitor()
        self.assertEqual(report["new_acoustic_detections"],0)
        self.assertEqual(report["acoustic_acceptance"],"PENDING_EXTERNAL_VOICE")

    def test_minimum_voice_count_required(self):
        with self.assertRaisesRegex(RuntimeError,"need 1"):
            self.run_monitor(minimum=1)
        report=self.run_monitor(acoustic=1,minimum=1)
        self.assertEqual(report["acoustic_acceptance"],"DETECTED_NOT_RATE_QUALIFIED")

    def test_missing_model_and_stalled_inference_fail(self):
        with self.assertRaisesRegex(RuntimeError,"not ready"):
            self.run_monitor(missing=True)
        with self.assertRaisesRegex(RuntimeError,"progress"):
            self.run_monitor(progress=False)

    def test_text_injection_cannot_pass_acoustic_monitor(self):
        with self.assertRaisesRegex(RuntimeError,"Text injection"):
            self.run_monitor(acoustic=1,text=1,minimum=1)

    def test_progress_is_not_enough_if_unarmed(self):
        with self.assertRaisesRegex(RuntimeError,"not armed"):
            self.run_monitor(armed=False)

    def test_guarded_or_lost_readiness_cannot_pass(self):
        with self.assertRaisesRegex(RuntimeError,"not ready"):
            self.run_monitor(guarded=True)
        with self.assertRaisesRegex(RuntimeError,"readiness lost"):
            self.run_monitor(readiness_lost=True)

    def test_reset_or_fault_cannot_pass_with_detections(self):
        with self.assertRaisesRegex(RuntimeError,"Unexpected reset"):
            self.run_monitor(acoustic=1,minimum=1,reboot=True)
        with self.assertRaisesRegex(RuntimeError,"P4 fault"):
            self.run_monitor(acoustic=1,minimum=1,fault=True)

    def test_transient_rebuild_must_recover(self):
        self.assertEqual(self.run_monitor(rebuilding=True)["final"]["loaded"], "1")
        with self.assertRaisesRegex(RuntimeError,"not ready at end"):
            self.run_monitor(stuck_rebuilding=True)


if __name__=="__main__":
    unittest.main()
