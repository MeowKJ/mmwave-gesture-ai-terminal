from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from probe_wifi_sdio import collect_probe, parse_status
import probe_wifi_sdio as probe


def status(token='abc', probe='abc', result='SDIO_R4', ocr='90ff8000', error='0', funcs=1, ready=1,
           idle='0'):
    return (f'MEO:WIFI token={token} probe={probe} state=DONE result={result} error=0x{error} '
            f'ocr=0x{ocr} funcs={funcs} ready={ready} elapsed_ms=2 cleanup=0x0 blocked=0 '
            f'hosted=NOT_VERIFIED wifi=UNAVAILABLE reset=0 idle=0x{idle}')


class Stream:
    def __init__(self, chunks):
        self.chunks, self.writes, self.now = list(chunks), [], 0

    def write(self, data):
        self.writes.append(data)

    def flush(self):
        pass

    def read(self, size):
        self.now += .1
        return self.chunks.pop(0) if self.chunks else b''


class WifiProbeTests(unittest.TestCase):
    def test_parse_complete(self):
        self.assertEqual(parse_status(status())['ocr'], 0x90ff8000)

    def test_no_response_is_not_identity(self):
        s = parse_status(status(result='NO_RESPONSE', ocr='00000000', error='107', funcs=0, ready=0))
        self.assertEqual(s['result'], 'NO_RESPONSE')

    def test_corrupt_records_not_salvaged(self):
        s = status()
        for text in (s[:-1], 'x' + s, s + 'FLS2:PONG', s.replace('reset=0', 'reset=1'),
                     s.replace('NOT_VERIFIED', 'VERIFIED'), s.replace('UNAVAILABLE', 'CONNECTED'),
                     s.replace('probe=abc', 'probe=a-b'), s.replace('idle=0x0', 'idle=zz')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_status(text)

    def test_idle_command_error_is_reported_not_fatal(self):
        s = parse_status(status(result='NO_RESPONSE', ocr='00000000', error='107', funcs=0,
                                ready=0, idle='103'))
        self.assertEqual((s['result'], s['error'], s['idle']), ('NO_RESPONSE', 0x107, 0x103))

    def test_r4_consistency(self):
        for s in (status(funcs=2), status(ready=0), status(ocr='ffffffff', funcs=7),
                  status(error='107'), status(ocr='90000000'),
                  status(result='PENDING'), status(ocr='80ff8000', funcs=0)):
            with self.subTest(text=s), self.assertRaises(ValueError):
                parse_status(s)

    def test_split_record_and_lost_ack_do_not_resend_probe(self):
        line = (status() + '\n').encode()
        stream = Stream([b'log\n', line[:30], line[30:]])
        s = collect_probe(stream, 'abc', {'lines': []}, clock=lambda: stream.now)
        self.assertEqual(s['result'], 'SDIO_R4')
        self.assertEqual(stream.writes.count(b'WIFI PROBE abc\n'), 1)

    def test_stale_tokens_not_accepted(self):
        stream = Stream([(status(token='old') + '\n' + status(probe='old') + '\n').encode()])
        with self.assertRaises(TimeoutError):
            collect_probe(stream, 'abc', {'lines': []}, timeout=1, clock=lambda: stream.now)
        self.assertEqual(stream.writes.count(b'WIFI PROBE abc\n'), 1)

    def test_busy_rejection(self):
        stream = Stream([('MEO:WIFI_ACCEPT token=abc accepted=0\n' + status(probe='other') + '\n').encode()])
        with self.assertRaises(RuntimeError):
            collect_probe(stream, 'abc', {'lines': []}, clock=lambda: stream.now)

    def test_invalid_token_does_not_write(self):
        for token in ('', 'a' * 17, 'x\nDOWNLOAD', 'a-b'):
            stream = Stream([])
            with self.subTest(token=token), self.assertRaises(ValueError):
                collect_probe(stream, token, {'lines': []})
            self.assertEqual(stream.writes, [])

    def test_no_newline_output_is_bounded(self):
        stream = Stream([b'x' * 4097])
        with self.assertRaises(ValueError):
            collect_probe(stream, 'abc', {'lines': []}, clock=lambda: stream.now)

    def run_main(self, result, *, changed_boot=False, fault='0'):
        app = {'port': 'COM10', 'reset_reason': 3, 'marker_retained': True, 'boot_id': '12345678'}
        after = dict(app, boot_id='87654321') if changed_boot else app
        with mock.patch.object(sys, 'argv', ['probe_wifi_sdio.py']), \
             mock.patch.object(probe.usb, 'image_identity', return_value={'elf_sha': 'a' * 64}), \
             mock.patch.object(probe.usb, 'check_application', side_effect=[app, after]), \
             mock.patch.object(probe.usb, 'open_native'), \
             mock.patch.object(probe, 'collect_probe', return_value=result), \
             mock.patch.object(probe.usb, 'read_fault_diagnostic', return_value={'valid': fault}), \
             mock.patch.object(probe.usb, 'write_report') as write_report, \
             mock.patch.object(probe.usb, 'flash_once') as flash, \
             mock.patch.object(probe.usb, 'send_magic') as download, \
             mock.patch.object(probe.usb, 'cpu_reset_from_rom') as reset:
            rc = probe.main()
            flash.assert_not_called()
            download.assert_not_called()
            reset.assert_not_called()
            return rc, write_report.call_args.args[1]

    def test_main_timeout_response_is_completed_not_wifi_acceptance(self):
        rc, report = self.run_main(parse_status(status(result='NO_RESPONSE', ocr='00000000',
                                                      error='107', funcs=0, ready=0)))
        self.assertEqual(rc, 0)
        self.assertTrue(report['probe_completed'])
        self.assertFalse(report['sdio_response_observed'])
        self.assertEqual(report['hosted'], 'NOT_VERIFIED')
        self.assertEqual(report['wifi'], 'UNAVAILABLE')

    def test_main_rejects_reboot(self):
        rc, report = self.run_main(parse_status(status()), changed_boot=True)
        self.assertEqual(rc, 1)
        self.assertFalse(report['ok'])

    def test_main_rejects_cleanup_or_blocked(self):
        for field, value in (('cleanup', 0x101), ('blocked', 1)):
            result = parse_status(status())
            result[field] = value
            rc, report = self.run_main(result)
            self.assertEqual(rc, 1)
            self.assertFalse(report['ok'])

    def test_main_requires_clear_fault_record(self):
        for fault in ('1', None):
            rc, report = self.run_main(parse_status(status()), fault=fault)
            self.assertEqual(rc, 1)
            self.assertFalse(report['ok'])


if __name__ == '__main__':
    unittest.main()
