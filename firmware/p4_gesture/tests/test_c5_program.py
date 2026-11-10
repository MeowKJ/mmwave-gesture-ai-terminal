"""Do not flash a stale/mismatched payload or retry a destructive command."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('c5_program_test_module', Path(__file__).resolve().parents[3]/'tools/c5_program.py')
program = importlib.util.module_from_spec(spec)
spec.loader.exec_module(program)
SHA = 'a'*64
TOKEN = '123456789abc'


def record(state='CONNECTED', stage='READY', error=0, chip=6, size=4194304, written=0, total=0, verified=0, token=TOKEN, operation=TOKEN, sha=SHA):
    return (f'MEO:C5_LOADER token={token} operation={operation} state={state} stage={stage} error={error} '
            f'chip={chip} flash_size={size} written={written} total={total} verified={verified} payload={sha} reset=never')


class Stream:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []
    def write(self, data): self.writes.append(data)
    def flush(self): pass
    def read(self, count): return self.chunks.pop(0) if self.chunks else b''


class C5ProgramTests(unittest.TestCase):
    def test_explicit_bootconnect_sent_once(self):
        response=record().replace('reset=never','reset=once').encode()+b'\n'
        stream=Stream([response])
        with patch.object(program.secrets,'token_hex',return_value=TOKEN):
            result=program.exchange(stream,'BOOTCONNECT',SHA,{'lines':[]},1)
        self.assertEqual(result['reset'],'once')
        self.assertEqual(stream.writes.count(f'C5 BOOTCONNECT {TOKEN}\n'.encode()),1)

    def test_reset_guard_requires_actual_radar_zero(self):
        report={}
        stream=Stream([b'MEO:UI page=eyes radar=0 brightness=100\n'])
        program.require_radar_inactive(stream,report)
        self.assertIn('radar=0',report['reset_guard_ui'])
        with self.assertRaisesRegex(RuntimeError,'Radar'):
            program.require_radar_inactive(Stream([b'MEO:UI page=eyes radar=1\n']),{})

    def test_facts_revision_and_nonce(self):
        facts=(f'MEO:C5_FACTS token={TOKEN} valid=1 rev_raw=0x00000012 '
               'strap=0x00000000 lp_sys=0x00000000 reset_cause=0x00000001 reset_completed=1\n')
        stream=Stream([facts.replace(TOKEN,'old').encode(),facts.encode()])
        with patch.object(program.secrets,'token_hex',return_value=TOKEN):
            result=program.read_facts(stream,{})
        self.assertEqual(result['revision_full'],102)
        self.assertTrue(result['reset_completed'])
        self.assertEqual(result['sample_source'],'CONNECT_CACHE')
        self.assertFalse(result['live_gpio_read'])

    def test_fresh_facts_reject_reused_connection_without_reset(self):
        program.require_new_facts_connection('IDLE')
        for state in ('CONNECTED','DONE','FAILED','CONNECTING'):
            with self.subTest(state=state), self.assertRaisesRegex(RuntimeError,'existing cache is not a live GPIO read'):
                program.require_new_facts_connection(state)

    def test_boot_pin_scope_does_not_claim_live_read(self):
        result=program.decode_boot_pins(0,0,[0x200,0x300,0x300],0x22)
        self.assertIn('CONNECT-time',result['scope'])
        self.assertIn('not live GPIO',result['scope'])

    def test_invalid_facts_not_accepted(self):
        facts=(f'MEO:C5_FACTS token={TOKEN} valid=0 rev_raw=0x00000000 '
               'strap=0x00000000 lp_sys=0x00000000 reset_cause=0x00000000 reset_completed=0\n')
        with patch.object(program.secrets,'token_hex',return_value=TOKEN), \
             self.assertRaisesRegex(RuntimeError,'not valid'):
            program.read_facts(Stream([facts.encode()]),{})

    def test_boot_pins_disabled_input_is_unknown_not_low(self):
        result=program.decode_boot_pins(0,0,[0,0x300,0x300],0x22)
        self.assertIsNone(result['pins']['26']['level'])
        self.assertEqual(result['pins']['28']['level'],0)
        self.assertTrue(result['pins']['28']['pull_up'])
        self.assertEqual(result['latched_gpio28'],0)

    def test_boot_pin_snapshot_does_not_replace_strap(self):
        result=program.decode_boot_pins(1<<28,0,[0x200,0x300,0x300],0x22)
        self.assertEqual(result['pins']['28']['level'],1)
        self.assertEqual(result['latched_gpio28'],0)
        self.assertFalse(result['pins']['28']['gpio_output_enabled'])

    def test_extended_facts_preserve_revision_and_pin_inputs(self):
        line=(f'MEO:C5_FACTS token={TOKEN} valid=1 rev_raw=0x25012412 strap=0x00000022 '
              'lp_sys=0x00000000 reset_cause=0x00000021 reset_completed=0 '
              'gpio_in=0x10000000 gpio_enable=0x00000000 mux26=0x00000200 '
              'mux27=0x00000300 mux28=0x00000300\n')
        with patch.object(program.secrets,'token_hex',return_value=TOKEN):
            facts=program.read_facts(Stream([line.encode()]),{})
        self.assertEqual(facts['revision_full'],102)
        self.assertEqual(facts['boot_pins']['pins']['28']['level'],1)
        self.assertEqual(facts['boot_pins']['latched_gpio28'],0)

    def test_connected_requires_actual_c5_and_flash_size(self):
        for changes in ({'chip': 8}, {'size': 2097152}, {'error': 1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                program.parse_status(record(**changes), SHA)

    def test_payload_mismatch_rejected_before_flash(self):
        with self.assertRaisesRegex(RuntimeError, 'payload'):
            program.parse_status(record(sha='b'*64), SHA)

    def test_completion_requires_full_progress_and_verification(self):
        for written, total, verified in ((0,0,1), (4,8,1), (8,8,0), (12,8,1)):
            with self.subTest(written=written,total=total,verified=verified), self.assertRaises(ValueError):
                program.parse_status(record(state='DONE', written=written,total=total,verified=verified), SHA)
        self.assertEqual(program.parse_status(record(state='DONE',written=8,total=8,verified=1),SHA)['verified'], 1)

    def test_stage_names_containing_digits_are_valid(self):
        # VERIFY_MD5 is emitted right after the last write; a [A-Z_] pattern
        # rejected it as malformed and aborted an already-verified flash.
        for stage, state in (('VERIFY_MD5', 'WRITING'), ('VERIFIED', 'DONE')):
            done = stage == 'VERIFIED'
            s = program.parse_status(record(state=state, stage=stage,
                                            written=8 if done else 8,
                                            total=8, verified=1 if done else 0), SHA)
            self.assertEqual(s['stage'], stage)

    def test_malformed_or_appended_records_rejected(self):
        for line in (record()+' garbage', record().replace(' reset=never',''), 'log '+record()):
            with self.subTest(line=line), self.assertRaises(ValueError):
                program.parse_status(line, SHA)

    def test_lost_ack_split_record_does_not_repeat_flash(self):
        result = record(state='DONE', stage='VERIFIED', written=8, total=8, verified=1).encode()+b'\n'
        stream = Stream([result[:80],result[80:]])
        with patch.object(program.secrets,'token_hex',return_value=TOKEN):
            s = program.exchange(stream,'FLASH',SHA,{'lines':[]},1)
        self.assertEqual(s['verified'],1)
        self.assertEqual(sum(x.startswith(b'C5 FLASH ') for x in stream.writes),1)

    def test_rejection_never_retries(self):
        stream = Stream([f'MEO:C5_LOADER_ACCEPT token={TOKEN} accepted=0\n'.encode()])
        with patch.object(program.secrets,'token_hex',return_value=TOKEN), self.assertRaisesRegex(RuntimeError,'rejected'):
            program.exchange(stream,'FLASH',SHA,{'lines':[]},1)
        self.assertEqual(sum(x.startswith(b'C5 FLASH ') for x in stream.writes),1)

    def test_stale_operation_not_accepted(self):
        wrong = record(state='DONE',written=8,total=8,verified=1,operation='old').encode()+b'\n'
        failed = record(state='FAILED',stage='CONNECT',error=2).encode()+b'\n'
        stream = Stream([wrong,failed])
        with patch.object(program.secrets,'token_hex',return_value=TOKEN), self.assertRaisesRegex(RuntimeError,'failed at CONNECT'):
            program.exchange(stream,'CONNECT',SHA,{'lines':[]},1)
        self.assertEqual(sum(x.startswith(b'C5 CONNECT ') for x in stream.writes),1)


if __name__ == '__main__': unittest.main()
