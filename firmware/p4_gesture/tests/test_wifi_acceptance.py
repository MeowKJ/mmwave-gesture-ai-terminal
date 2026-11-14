"""No-board tests for fresh scan evidence and one-shot native USB commands."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"tools"))
import wifi_acceptance as w


class Stream:
    def __init__(self, lines): self.lines=lines;self.writes=[]
    def write(self,data): self.writes.append(data)
    def flush(self): pass
    def read(self,size):
        del size
        return self.lines.pop(0) if self.lines else b""


class WifiAcceptanceTests(unittest.TestCase):
    def test_split_fresh_response_ignores_old_tokens(self):
        s=Stream([b"MEO:WIFI_ACCEPT token=old queued=1\nMEO:WIFI_ACCEPT token=abc qu",
                  b"eued=1\n"])
        with patch.object(w.secrets,"token_hex",return_value="abc"):
            self.assertEqual(w.Link(s).command("SCAN","MEO:WIFI_ACCEPT")["queued"],"1")
        self.assertEqual(s.writes,[b"WIFI SCAN abc\n"])

    def test_missing_ack_does_not_replay_scan(self):
        s=Stream([])
        with patch.object(w.time,"monotonic",side_effect=[0,0,4]), \
                patch.object(w.secrets,"token_hex",return_value="abc"),self.assertRaises(TimeoutError):
            w.Link(s).command("SCAN","MEO:WIFI_ACCEPT")
        self.assertEqual(s.writes,[b"WIFI SCAN abc\n"])

    def test_only_committed_current_generation_can_complete(self):
        status=dict(scan_seq="2",scan_done="1",scanning="1",scan_error="0")
        self.assertFalse(w.completed_scan(status,2))
        status.update(scan_done="2",scanning="0")
        self.assertTrue(w.completed_scan(status,2))
        with self.assertRaises(RuntimeError):w.completed_scan(status,1)
        status["scan_error"]="263"
        with self.assertRaises(RuntimeError):w.completed_scan(status,2)

    def test_wrong_elf_refuses_before_radio_commands(self):
        s=Stream([])
        with patch.object(w.usb,"query",return_value=("abc","a"*64,"1","12","12345678")), \
                self.assertRaisesRegex(RuntimeError,"ELF"):
            w.run(s,"b"*64,3,{})
        self.assertEqual(s.writes,[])

    def test_fault_refuses_before_radio_commands(self):
        s=Stream([])
        with patch.object(w.usb,"query",side_effect=[("abc","a"*64,"1","12","12345678"),
                ("p","1","1","0","0"),("f","1")]), self.assertRaisesRegex(RuntimeError,"guard"):
            w.run(s,"a"*64,3,{})
        self.assertEqual(s.writes,[])


if __name__=="__main__":unittest.main()
