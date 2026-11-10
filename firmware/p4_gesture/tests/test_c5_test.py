"""Trust fresh Hosted evidence; never repeat a mutating diagnostic command."""
import argparse
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools'))
import c5_test as diagnostic


def status(token='abc', operation='abc', state='FAILED', width=1, khz=400):
    ready = state == 'READY'
    return (f'MEO:WIFI_HOSTED token={token} state={state} error={"0x0" if ready else "0xffffffff"} '
            f'chip_id=23 chip={"esp32c5" if ready else "unknown"} '
            f'fw={"2.12.13" if ready else "unknown:0.0.0"} rev=0 pre=0 build=0 '
            f'wifi_started={int(ready)} gpio6=UNUSED reset_gpio=54 proto=2 '
            f'operation={operation} diag=1 width={width} khz={khz} poll_ms=0 init_ms=4000')


class Stream:
    def __init__(self, chunks):
        self.chunks, self.writes, self.now = list(chunks), [], 0
    def write(self, value): self.writes.append(value)
    def flush(self): pass
    def read(self, size):
        self.now += .1
        return self.chunks.pop(0) if self.chunks else b''


class C5DiagnosticTests(unittest.TestCase):
    def test_matrix_is_bounded_and_varies_one_factor(self):
        self.assertEqual(len(diagnostic.MATRIX), 6)
        for width, khz in diagnostic.MATRIX:
            self.assertEqual(diagnostic.parse_case(f'{width}:{khz}'), (width, khz))

    def test_invalid_cases_rejected(self):
        for case in ('4:0', '2:400', '1:500000', '4:-400', '4:400\nRESET', '4x400'):
            with self.subTest(case=case), self.assertRaises(argparse.ArgumentTypeError):
                diagnostic.parse_case(case)

    def test_parse_and_invalid_protocol_fields(self):
        self.assertEqual(diagnostic.parse_status(status())['width'], 1)
        for line in (status() + ' bad', status() + ' width=4',
                     status().replace('proto=2', 'proto=1'),
                     status().replace('diag=1', 'diag=7'),
                     status().replace('khz=400', 'khz=0'),
                     status().replace('poll_ms=0', 'poll_ms=500'),
                     status().replace('init_ms=4000', 'init_ms=21000'),
                     status().replace('init_ms=4000', 'init_ms=4100'),
                     status().replace('state=FAILED', 'state=BOGUS'),
                     status().replace('gpio6=UNUSED', 'gpio6=OUTPUT'),
                     status().replace('reset_gpio=54', 'reset_gpio=6')):
            with self.subTest(line=line), self.assertRaises(ValueError):
                diagnostic.parse_status(line)

    def test_ready_needs_identity_version_and_wifi(self):
        good = status(state='READY')
        self.assertEqual(diagnostic.parse_status(good)['state'], 'READY')
        for bad in (good.replace('esp32c5', 'esp32c6'),
                    good.replace('fw=2.12.13', 'fw=unknown:0.0.0'),
                    good.replace('wifi_started=1', 'wifi_started=0'),
                    good.replace('error=0x0', 'error=0x107')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                diagnostic.parse_status(bad)

    def test_split_output_lost_ack_no_repeat_connect(self):
        data = (status() + '\n').encode()
        stream = Stream([data[:20], data[20:]])
        result = diagnostic.exchange(stream, 'WIFI HOSTED CONNECT abc', 'abc', {'lines': []},
            timeout=1, predicate=lambda s: s['operation'] == 'abc' and s['state'] == 'FAILED',
            clock=lambda: stream.now)
        self.assertEqual(result['state'], 'FAILED')
        self.assertEqual(stream.writes.count(b'WIFI HOSTED CONNECT abc\n'), 1)

    def test_stale_status_or_operation_not_accepted(self):
        stream = Stream([(status(token='old') + '\n' + status(operation='old') + '\n').encode()])
        with self.assertRaises(TimeoutError):
            diagnostic.exchange(stream, 'WIFI HOSTED CONNECT abc', 'abc', {'lines': []},
                timeout=1, predicate=lambda s: s['operation'] == 'abc', clock=lambda: stream.now)
        self.assertEqual(stream.writes.count(b'WIFI HOSTED CONNECT abc\n'), 1)

    def test_rejected_config_stops_without_retry(self):
        stream = Stream([b'MEO:WIFI_HOSTED_CONFIG token=abc accepted=0 error=0x103 width=1 khz=400\n'])
        with self.assertRaisesRegex(RuntimeError, 'rejected'):
            diagnostic.exchange(stream, 'WIFI HOSTED CONFIG abc 1 400', 'abc', {'lines': []},
                timeout=1, predicate=lambda s: True, clock=lambda: stream.now)
        self.assertEqual(stream.writes.count(b'WIFI HOSTED CONFIG abc 1 400\n'), 1)

    def test_invalid_token_does_not_touch_usb(self):
        stream = Stream([])
        with self.assertRaises(ValueError):
            diagnostic.exchange(stream, 'CONNECT', 'a\nRESET', {'lines': []},
                timeout=1, predicate=lambda s: True)
        self.assertEqual(stream.writes, [])

    def test_bounded_output(self):
        stream = Stream([b'x' * 8193])
        with self.assertRaises(ValueError):
            diagnostic.exchange(stream, None, 'abc', {'lines': []},
                timeout=1, predicate=lambda s: True, clock=lambda: stream.now)

    def test_init_wait_timeout_is_not_sdio_command_timeout(self):
        evidence = diagnostic.summarize_evidence([
            status().replace('0xffffffff', '0x107'),
            'E transport: no Hosted init event within 4000 ms; no retry/reset',
            'I H_SDIO_DRV: MEO_SDIO_TEST: card initialized',
            'W H_SDIO_DRV: MEO_SDIO_TEST: one slave reset; one card initialization'])
        self.assertTrue(evidence['card_initialized'])
        self.assertTrue(evidence['hosted_init_timeout'])
        self.assertEqual(evidence['sdio_command_timeouts'], 0)
        self.assertEqual(evidence['reset_sequences'], 1)

    def test_actual_crc_and_sdio_timeout_records(self):
        evidence = diagnostic.summarize_evidence([
            'E sdmmc_io: read returned 0x109',
            'E SD_HOST: command returned 0x107'])
        self.assertEqual(evidence['crc_errors'], 1)
        self.assertEqual(evidence['sdio_command_timeouts'], 1)
        self.assertFalse(evidence['hosted_init_timeout'])

    def test_extended_init_timeout_is_not_bus_timeout(self):
        evidence = diagnostic.summarize_evidence([
            'E transport: no Hosted init event within 20000 ms; no retry/reset'])
        self.assertTrue(evidence['hosted_init_timeout'])
        self.assertEqual(evidence['sdio_command_timeouts'], 0)

    def test_snapshot_zero_requires_complete_successful_reads(self):
        sample = ('I sdio_wrapper: MEO_SDIO_SNAPSHOT error=0x0 ocr=0xa0ffff00 '
                  'funcs=2 ien=0x03 pending=0x00 token=0x00000000 raw=0x00000000 '
                  'st=0x00000000 len=0x00000000 ena=0x00800040')
        evidence = diagnostic.summarize_evidence([sample] * 20)
        self.assertEqual(evidence['snapshot_valid_reads'], 20)
        self.assertTrue(evidence['snapshot_data_all_zero'])
        for lines in ([], [sample.replace('token=0x00000000', 'token=0x00140000')],
                      [sample, sample.replace('error=0x0', 'error=0x107')],
                      [sample, sample.replace(' len=0x00000000', '')],
                      [sample, sample + ' token=0x0']):
            with self.subTest(lines=lines):
                self.assertFalse(diagnostic.summarize_evidence(lines)['snapshot_data_all_zero'])
        self.assertEqual(diagnostic.summarize_evidence([
            sample.replace('error=0x0', 'error=0x107')])['snapshot_read_errors'], 1)

    def test_invalid_extended_options_rejected_before_usb(self):
        for args in (['--poll-ms', '50'], ['--init-ms', '3999'],
                     ['--init-ms', '21000'], ['--init-ms', '4100'],
                     ['--init-ms', '20000', '--timeout', '31']):
            with self.subTest(args=args), \
                 patch.object(diagnostic.usb, 'check_application') as usb_check, \
                 self.assertRaises(SystemExit) as result:
                diagnostic.main(args)
            self.assertEqual(result.exception.code, 2)
            usb_check.assert_not_called()

    def test_entry_point_forwards_extended_options(self):
        with patch.object(sys, 'argv', ['meoave.py', 'c5-test', '--case', '1:400',
                '--poll-ms', '100', '--init-ms', '20000']), \
             patch.object(diagnostic.meoave.subprocess, 'run') as run:
            run.return_value.returncode = 2
            self.assertEqual(diagnostic.meoave.main(), 2)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index('--poll-ms') + 1], '100')
        self.assertEqual(command[command.index('--init-ms') + 1], '20000')
        self.assertEqual(command[command.index('--case') + 1], '1:400')

    def test_postflight_rejects_non_idle_or_leftover_diagnostic(self):
        for state, diag in (('CONNECTING', 0), ('IDLE', 1)):
            with self.subTest(state=state, diag=diag), \
                 patch.object(diagnostic.usb, 'check_application', return_value={'port': 'COM10'}), \
                 patch.object(diagnostic.usb, 'open_native'), \
                 patch.object(diagnostic, 'exchange', return_value={'state': state, 'diag': diag}), \
                 self.assertRaises(RuntimeError):
                diagnostic.postflight('a'*64, {})

    def test_postflight_requires_clear_fault_and_checks_runtime(self):
        for valid in ('0', '1'):
            with self.subTest(valid=valid), \
                 patch.object(diagnostic.usb, 'check_application', return_value={'port': 'COM10'}), \
                 patch.object(diagnostic.usb, 'open_native'), \
                 patch.object(diagnostic, 'exchange', return_value={'state': 'IDLE', 'diag': 0}), \
                 patch.object(diagnostic.usb, 'read_fault_diagnostic', return_value={'valid': valid}), \
                 patch.object(diagnostic.usb, 'check_fls2_runtime') as runtime:
                if valid == '0':
                    result = {}
                    diagnostic.postflight('a'*64, result)
                    self.assertTrue(result['ok'])
                    runtime.assert_called_once()
                else:
                    with self.assertRaises(RuntimeError):
                        diagnostic.postflight('a'*64, {})
                    runtime.assert_not_called()

    def test_timeout_stops_matrix_not_blind_reboot(self):
        failed = {'width': 1, 'khz': 400, 'ok': False, 'completed': False,
                  'error': 'timeout', 'evidence': {}, 'lines': []}
        with patch.object(diagnostic.usb, 'image_identity', return_value={'elf_sha': 'a'*64}), \
             patch.object(diagnostic.usb, 'check_application', return_value={'port': 'COM10'}), \
             patch.object(diagnostic.usb, 'open_native') as opened, \
             patch.object(diagnostic, 'run_case', return_value=failed) as case, \
             patch.object(diagnostic.usb, 'write_report'), \
             patch.object(diagnostic.usb, 'restart_once') as restart, \
             patch.object(diagnostic.usb, 'flash_once') as flash:
            opened.return_value.__enter__.return_value.read.return_value = b'MEO:UI radar=0\n'
            self.assertEqual(diagnostic.main([]), 1)
            case.assert_called_once()
            restart.assert_not_called()
            flash.assert_not_called()


if __name__ == '__main__':
    unittest.main()
