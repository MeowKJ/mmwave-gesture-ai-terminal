"""Application evidence must include identity, real progress and nonce echo counts."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
import c5_minimal as minimal
import meoave

SHA='a'*64
TOKEN='123456789abc'


def record(**changes):
    d=dict(token=TOKEN,operation=TOKEN,state='PASS',stage='VERIFIED',error=0,
        reset_completed=1,samples=20,echoes=16,hb_first=100,hb_last=150,reason=1,elf_sha=SHA)
    d.update(changes)
    return 'MEO:C5_MINIMAL '+' '.join(f'{k}={v}' for k,v in d.items())


class Stream:
    def __init__(self,chunks): self.chunks=list(chunks);self.writes=[]
    def write(self,data): self.writes.append(data)
    def flush(self): pass
    def read(self,size): return self.chunks.pop(0) if self.chunks else b''


class MinimalTests(unittest.TestCase):
    def test_complete_real_evidence(self):
        self.assertEqual(minimal.parse_status(record(),SHA)['echoes'],16)

    def test_missing_progress_or_wrong_identity_never_passes(self):
        for changes in ({'echoes':15},{'samples':19},{'hb_last':100},{'error':1},
                        {'reset_completed':0},{'elf_sha':'none'},{'elf_sha':'b'*64}):
            with self.subTest(changes=changes), self.assertRaises((ValueError,RuntimeError)):
                minimal.parse_status(record(**changes),SHA)

    def test_run_sent_once_lost_ack_and_stale_nonce(self):
        chunks=[(record(operation='old')+'\n').encode(),(record()+'\n').encode()]
        stream=Stream(chunks)
        with patch.object(minimal.secrets,'token_hex',return_value=TOKEN):
            minimal.exchange(stream,SHA,{'lines':[]},run=True)
        self.assertEqual(stream.writes.count(f'C5 DIAGRUN {TOKEN}\n'.encode()),1)

    def test_reject_or_failure_does_not_reset_again(self):
        for response in (f'MEO:C5_MINIMAL_ACCEPT token={TOKEN} accepted=0',
                         record(state='FAILED',stage='APP_SIGNATURE',error=263,elf_sha='none',samples=0,echoes=0)):
            stream=Stream([(response+'\n').encode()])
            with patch.object(minimal.secrets,'token_hex',return_value=TOKEN),self.assertRaises(RuntimeError):
                minimal.exchange(stream,SHA,{'lines':[]},run=True)
            self.assertEqual(sum(x.startswith(b'C5 DIAGRUN ') for x in stream.writes),1)

    def test_app_only_layout_and_stale_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=minimal.stage_payload(Path(tmp),b'\xe9abc'*8,'diagnostic','a'*64)
            self.assertIn('C5_PAYLOAD_IS_DIAG 1',meoave.programmer_payload_layout(path))
            (path.parent/'application.bin').write_bytes(b'different'*4)
            with self.assertRaisesRegex(RuntimeError,'stale'):
                meoave.programmer_payload_layout(path)

    def test_cannot_change_bootloader_or_exceed_partition(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=minimal.stage_payload(Path(tmp),b'1234','restore','a'*64)
            self.assertIn('C5_PAYLOAD_IS_DIAG 0',meoave.programmer_payload_layout(path))
            manifest=path.parent/'flasher_args.json'
            info=json.loads(manifest.read_text());info['flash_files']={'0x2000':'application.bin'}
            manifest.write_text(json.dumps(info))
            with self.assertRaisesRegex(RuntimeError,'app-only'):
                meoave.programmer_payload_layout(path)

    def test_run_requires_confirmed_release_before_hardware(self):
        class Args: straps_released=False
        with self.assertRaisesRegex(ValueError,'straps-released'):
            minimal.run_test({},Args())

    def test_cli_forwards_minimal_flags(self):
        with patch.object(meoave,'load_config',return_value={}), \
             patch.object(meoave.subprocess,'run') as run, \
             patch.object(sys,'argv',['meoave.py','c5-minimal','--run','--straps-released']):
            meoave.main()
        self.assertIn('--straps-released',run.call_args.args[0])
        self.assertIn('--run',run.call_args.args[0])

    def test_passive_capture_not_app_acceptance(self):
        r=record(state='OBSERVED',stage='CAPTURED',reset_completed=0,
                 samples=0,echoes=0,hb_first=0,hb_last=0,elf_sha='none')
        s=minimal.parse_status(r,SHA,observe=True)
        self.assertEqual(s['state'],'OBSERVED')
        for changes in ({'reset_completed':1},{'echoes':1},{'error':263}):
            with self.subTest(changes=changes),self.assertRaises(ValueError):
                minimal.parse_status(record(state='OBSERVED',**changes),SHA,observe=True)

    def test_passive_command_sent_once_never_resets(self):
        stream=Stream([(record(state='OBSERVED',stage='CAPTURED',reset_completed=0,
            samples=0,echoes=0,hb_first=0,hb_last=0,elf_sha='none')+'\n').encode()])
        with patch.object(minimal.secrets,'token_hex',return_value=TOKEN):
            minimal.exchange(stream,SHA,{'lines':[]},run=True,observe=True)
        self.assertEqual(sum(x.startswith(b'C5 DIAGOBSERVE ') for x in stream.writes),1)
        self.assertFalse(any(x.startswith(b'C5 DIAGRUN ') for x in stream.writes))

    def test_cached_trace_nonce_and_complete_bytes(self):
        line=f'MEO:C5_TRACE token={TOKEN} operation={TOKEN} index=0 count=1 bytes=12 error=0 uptime_ms=100 raw='+'0'*24+'\n'
        stream=Stream([line.replace(f'token={TOKEN}','token=old').encode(),line.encode()])
        report={}
        with patch.object(minimal.secrets,'token_hex',return_value=TOKEN):
            traces=minimal.read_traces(stream,TOKEN,report,expected_count=1)
        summary=minimal.summarize_traces(traces)
        self.assertTrue(summary['all_zero'])
        self.assertEqual(summary['complete_reads'],1)
        self.assertEqual(summary['app_execution'],'NOT_VERIFIED')
        self.assertTrue(all(x.startswith(b'C5 DIAGTRACE ') for x in stream.writes))

    def test_partial_read_never_presented_as_zero_capture(self):
        rows=[{'bytes':12,'error':0,'raw':'0'*24},{'bytes':4,'error':263,'raw':'0'*24}]
        summary=minimal.summarize_traces(rows)
        self.assertFalse(summary['all_zero'])
        self.assertEqual(summary['complete_reads'],1)

    def test_trace_expected_count_is_enforced(self):
        line=f'MEO:C5_TRACE token={TOKEN} operation={TOKEN} index=0 count=1 bytes=12 error=0 uptime_ms=100 raw='+'0'*24+'\n'
        with patch.object(minimal.secrets,'token_hex',return_value=TOKEN),self.assertRaises(RuntimeError):
            minimal.read_traces(Stream([line.encode()]),TOKEN,{},expected_count=20)

    def test_reset_trace_requires_observed_low_and_high(self):
        line=(f'MEO:C5_RESET_TRACE token={TOKEN} operation={TOKEN} valid=1 error=0 '
              'before=1 low_begin=0 low_end=0 high_begin=1 high_end=1 low_us=10000 '
              'ie=1 oe=1 od=0 mux=1 signal=256 peripheral=0 sleep=0 scope=P4_PAD_ONLY')
        trace=minimal.parse_reset_trace(line)
        self.assertTrue(trace['pad_sequence_verified'])
        self.assertIn('C5 EN waveform/boot mode NOT_VERIFIED',trace['scope'])
        for old,new in (('low_begin=0','low_begin=1'),('low_end=0','low_end=-1'),
                        ('high_begin=1','high_begin=0'),('high_end=1','high_end=0'),
                        ('low_us=10000','low_us=0'),('valid=1','valid=0'),
                        ('error=0','error=1'),('ie=1','ie=0'),('oe=1','oe=0'),
                        ('od=0','od=1'),('mux=1','mux=0'),('signal=256','signal=17')):
            with self.subTest(new=new):
                self.assertFalse(minimal.parse_reset_trace(line.replace(old,new))['pad_sequence_verified'])
        self.assertTrue(minimal.parse_reset_trace(line.replace('peripheral=0','peripheral=1'))['pad_sequence_verified'])

    def test_reset_trace_ignores_stale_nonce_and_operation(self):
        line=(f'MEO:C5_RESET_TRACE token={TOKEN} operation={TOKEN} valid=1 error=0 '
              'before=1 low_begin=0 low_end=0 high_begin=1 high_end=1 low_us=10000 '
              'ie=1 oe=1 od=0 mux=1 signal=256 peripheral=0 sleep=0 scope=P4_PAD_ONLY\n')
        stream=Stream([line.replace(f'token={TOKEN}','token=old').encode(),
            line.replace(f'operation={TOKEN}','operation=old').encode(),line.encode()])
        with patch.object(minimal.secrets,'token_hex',return_value=TOKEN):
            result=minimal.read_reset_trace(stream,TOKEN)
        self.assertTrue(result['pad_sequence_verified'])
        self.assertTrue(all(x.startswith(b'C5 RESETTRACE ') for x in stream.writes))

    def test_reset_trace_timeout_only_queries_cache(self):
        stream=Stream([])
        ticks=iter([0,0,0,0,5])
        with self.assertRaises(TimeoutError):
            minimal.read_reset_trace(stream,TOKEN,clock=lambda:next(ticks))
        self.assertTrue(stream.writes)
        self.assertTrue(all(x.startswith(b'C5 RESETTRACE ') for x in stream.writes))


if __name__=='__main__': unittest.main()
