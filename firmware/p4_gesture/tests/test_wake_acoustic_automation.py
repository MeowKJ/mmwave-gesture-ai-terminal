"""Execute the production acoustic test against deterministic USB/speaker fakes.
These are host regressions, NOT acoustic/hardware acceptance.
"""
import copy
import base64
import importlib.util
import json
import re
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch
import wave

TOOLS=Path(__file__).resolve().parents[3]/'tools'
spec=importlib.util.spec_from_file_location('wake_acoustic_subject',TOOLS/'wake_acoustic_acceptance.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def fixture(path):
    with wave.open(str(path),'wb') as w:
        w.setparams((1,2,16000,0,'NONE','not compressed'))
        w.writeframes(b'\x01\x01'*3200)


class Clock:
    def __init__(self):self.now=1
    def monotonic(self):return self.now
    def sleep(self,n):self.now+=n


class Hardware:
    def __init__(self,clock):
        self.clock=clock;self.count=0;self.chunks=0;self.pending=b'';self.commands=[]
        self.event=False;self.ack_until=0;self.speech_until=0;self.fill_seq=0
        self.fail=False;self.text=0;self.played=0;self.drops=0
    def __enter__(self):return self
    def __exit__(self,*a):return False
    def write(self,data):
        command=data.decode().strip();self.commands.append(command)
        if command=='UI GO eyes':return len(data)
        if command.startswith('MIC STATUS '):
            self.pending+=('MEO:MIC token='+command.split()[-1]+' running=1 rms_l=-22 rms_r=-24\n').encode()
            return len(data)
        assert command=='UI'
        self.chunks+=4
        if self.event:
            self.pending+=f'MEO:WAKE source=ACOUSTIC_ESP_SR detected=1 count={self.count}\n'.encode();self.event=False
        ack=int(self.clock.now<self.ack_until)
        visible=int(self.clock.now<self.speech_until)
        if visible:self.fill_seq+=1
        self.pending+=(f'MEO:UI page=eyes eye=2 frames={self.chunks}\n'
          f'MEO:UI_AUDIO mic_running=1 wake_detected={ack} wake_detections={self.count}\n'
          f'MEO:UI_CAPTURE visible={visible} ack={ack*visible} seq={self.fill_seq} level={visible*60} fill_h={visible*6} ink=ccaa55\n'
          'MEO:WAKE_STATUS linked=1 loaded=1 keyword_ready=1 requested=1 armed=1 guarded=0 '
          f'error=NONE chunks={self.chunks} drops={self.drops} resets=0 acoustic={self.count} text_tests={self.text}\n').encode()
        return len(data)
    def read(self,n):
        self.clock.sleep(.01)
        data,self.pending=self.pending[:n],self.pending[n:]
        return data
    def flush(self):pass
    def start(self,path):
        self.played+=1
        if self.fail or self.played==getattr(self,'fail_at',-1):raise OSError('speaker unavailable')
        self.speech_until=self.clock.now+.7
        if path.name=='positive.wav':
            self.count+=1;self.event=True;self.ack_until=self.clock.now+1.8
            if getattr(self,'drop_on_wake',False):self.drops+=4
    def stop(self):pass


class AcousticTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.positive=self.root/'positive.wav';self.negative=self.root/'negative.wav'
        fixture(self.positive);fixture(self.negative)
        self.clock=Clock();self.hw=Hardware(self.clock)
        self.args=m.parser().parse_args(['--report',str(self.root/'result.json'),'--wav',str(self.positive),
           '--negative-wav',str(self.negative),'--repeat','2','--soak-seconds','0','--listen-after','2'])
        self.report={'ok':False}
        identity={'elf_sha':'a'*64}
        self.patches=[patch.object(m,'time',self.clock),patch.object(m,'Speaker',return_value=self.hw),
          patch.object(m.usb,'image_identity',return_value=identity),
          patch.object(m.usb,'check_application',return_value={'port':'COM10','boot_id':'same'}),
          patch.object(m.usb,'open_native',return_value=self.hw),
          patch.object(m.usb,'read_fault_diagnostic',return_value={'valid':'0'})]
        for p in self.patches:p.start();self.addCleanup(p.stop)

    def test_runs_real_flow_with_supplied_wavs_no_sapi_or_injection(self):
        with patch.object(m,'make_sapi_wav',side_effect=AssertionError('SAPI must not run')):
            m.run(self.args,self.report)
        self.assertTrue(self.report['ok']);self.assertEqual(self.report['successes'],2)
        self.assertEqual(len(self.report['controls']),4)
        self.assertEqual(self.hw.played,3)
        self.assertTrue(all(c=='UI' or c=='UI GO eyes' or c.startswith('MIC STATUS ') for c in self.hw.commands))
        self.assertTrue(all(a['detections']==1 and a['events'] and not a['errors'] for a in self.report['attempts']))
        self.assertTrue(any(s['capture']['ack']=='1' for s in self.report['attempts'][0]['samples']))
        self.assertEqual(self.report['queue_drops_delta'],0)
        self.assertEqual(self.report['warnings'],[])
        self.assertFalse(self.report['stability_qualified'])

    def test_failed_playback_keeps_partial_control_and_flash_evidence(self):
        self.hw.fail=True
        argv=['--report',str(self.root/'failure.json'),'--wav',str(self.positive),'--negative-wav',str(self.negative),
              '--flash','--soak-seconds','0']
        def flashed(*a,**kw):kw['report']['hash_verified']=True
        with patch.object(m.usb,'flash_once',side_effect=flashed),patch.object(m.usb,'app_port',return_value=None):
            self.assertEqual(m.main(argv),1)
        saved=json.loads((self.root/'failure.json').read_text(encoding='utf-8'))
        self.assertTrue(saved['flash']['hash_verified'])
        self.assertTrue(saved['transcript']);self.assertEqual(len(saved['controls']),2)
        self.assertIn('speaker unavailable',saved['error']);self.assertIn('finished',saved)

    def test_second_trial_failure_preserves_first_and_failed_trial(self):
        self.hw.fail_at=3
        argv=['--report',str(self.root/'trial-failed.json'),'--wav',str(self.positive),
              '--negative-wav',str(self.negative),'--repeat','2','--soak-seconds','0']
        with patch.object(m.usb,'app_port',return_value=None):
            self.assertEqual(m.main(argv),1)
        saved=json.loads((self.root/'trial-failed.json').read_text(encoding='utf-8'))
        self.assertEqual(saved['attempts'][0]['detections'],1)
        self.assertEqual(saved['attempts'][1]['number'],2)
        self.assertIn('finished',saved['attempts'][1]);self.assertFalse(saved['ok'])

    def test_flash_failure_is_preserved_and_never_retried(self):
        argv=['--report',str(self.root/'flash.json'),'--wav',str(self.positive),'--negative-wav',str(self.negative),'--flash']
        def failure(*a,**kw):
            kw['report']['written']=True
            raise RuntimeError('application validation failed')
        with patch.object(m.usb,'flash_once',side_effect=failure) as flash,patch.object(m.usb,'app_port',return_value=None):
            self.assertEqual(m.main(argv),1)
        self.assertEqual(flash.call_count,1)
        saved=json.loads((self.root/'flash.json').read_text(encoding='utf-8'))
        self.assertTrue(saved['flash']['written']);self.assertFalse(saved['ok'])

    def test_missing_event_cannot_pass_even_with_count_and_ui(self):
        m.run(self.args,self.report)
        trial=copy.deepcopy(self.report['attempts'][0]);trial['events']=[]
        self.assertIn('missing/unexpected acoustic event',m.assess(trial,self.report['initial'],True))

    def test_badge_is_input_only_and_quiet_does_not_require_visibility(self):
        m.run(self.args,self.report)
        self.assertEqual(self.report['initial']['capture']['visible'],'0')
        trial=copy.deepcopy(self.report['attempts'][0])
        # Wake feedback can legitimately outlast the last sound. It must not
        # force a visible microphone just to make a test pass.
        for sample in trial['samples']:sample['capture']['ack']='0'
        self.assertEqual(m.assess(trial,self.report['initial'],True),[])
        for sample in trial['samples']:sample['capture']['fill_h']='0'
        self.assertIn('input-driven microphone fill not observed during speech',
                      m.assess(trial,self.report['initial'],True))
        quiet=copy.deepcopy(self.report['controls'][0]);quiet['samples'][-1]['capture']['visible']='1'
        self.assertIn('listening indicator did not hide in silence',
                      m.assess(quiet,self.report['initial'],False))

    def test_quiet_false_wake_and_text_injection_rejected(self):
        m.run(self.args,self.report)
        trial=self.report['attempts'][0]
        self.assertIn('false wake in quiet/negative control',m.assess(trial,self.report['initial'],False))
        state=copy.deepcopy(self.report['initial']);state['wake']['text_tests']='1'
        with self.assertRaisesRegex(RuntimeError,'Text injection'):
            m.check_sample(state,self.report['initial'],{})

    def test_bounded_rebuild_and_stalled_inference(self):
        m.run(self.args,self.report)
        state=copy.deepcopy(self.report['initial']);state['wake']['error']='MODEL_REBUILDING'
        state['wake']['armed']='0';tracker={}
        m.check_sample(state,self.report['initial'],tracker)
        self.clock.sleep(3.1)
        with self.assertRaisesRegex(RuntimeError,'rebuilding exceeded'):
            m.check_sample(state,self.report['initial'],tracker)
        state['wake'].update(error='NONE',armed='1');tracker={}
        m.check_sample(state,self.report['initial'],tracker);self.clock.sleep(3.1)
        with self.assertRaisesRegex(RuntimeError,'Inference stalled'):
            m.check_sample(state,self.report['initial'],tracker)

    def test_link_rejects_text_reset_duplicate_and_oversized_records(self):
        link=m.Link(self.hw,self.report)
        for command in ('AI WAKE cats','SYS RESTART','DOWNLOAD','UI\nUI'):
            with self.assertRaises(ValueError):link.send(command)
        with self.assertRaises(ValueError):m.fields('MEO:WAKE armed=1 armed=0','MEO:WAKE')
        self.hw.pending=b'x'*17000
        with self.assertRaisesRegex(RuntimeError,'Oversized'):
            while True:link.readline(self.clock.now+1)

    def test_wrong_port_rejected_before_commands(self):
        self.args.port='COM3'
        with patch.object(m.usb,'select_port',side_effect=RuntimeError('non-native')):
            with self.assertRaisesRegex(RuntimeError,'non-native'):m.run(self.args,self.report)
        self.assertFalse(self.hw.commands)

    def test_unexpected_reset_cannot_pass_after_good_detections(self):
        with patch.object(m.usb,'check_application',side_effect=[{'port':'COM10','boot_id':'old'},
                                                               {'port':'COM10','boot_id':'new'}]):
            with self.assertRaisesRegex(RuntimeError,'unexpected reset'):
                m.run(self.args,self.report)
        self.assertFalse(self.report['ok']);self.assertEqual(self.report['successes'],2)

    def test_initial_model_loading_is_awaited_not_immediately_failed(self):
        link=m.Link(self.hw,self.report)
        live=link.status();loading=copy.deepcopy(live)
        loading['wake'].update(linked='1',loaded='0',keyword_ready='0',armed='0',error='MODEL_LOADING')
        with patch.object(link,'status',side_effect=[loading,live]) as status:
            self.assertTrue(m.ready(m.wait_ready(link,3)['wake']));self.assertEqual(status.call_count,2)

    def test_sapi_payload_is_data_not_powershell_code(self):
        target=self.root/"声源$`'test.wav"
        def synthesize(command,**kw):
            self.assertEqual(kw['timeout'],45)
            script=base64.b64decode(command[-1]).decode('utf-16le')
            data=re.search(r"FromBase64String\('([^']+)'\)",script)[1]
            payload=json.loads(base64.b64decode(data).decode('utf-8'))
            self.assertEqual(payload['path'],str(target.resolve()))
            self.assertEqual(payload['phrase'],'Hi，喵喵')
            self.assertNotIn(str(target),script);fixture(target)
            return subprocess.CompletedProcess(command,0,'{"voice":"Huihui","culture":"zh-CN"}','')
        with patch.object(m.subprocess,'run',side_effect=synthesize):
            self.assertEqual(m.make_sapi_wav(target,'Hi，喵喵')['culture'],'zh-CN')

    def test_truncated_or_too_long_wav_rejected(self):
        self.assertGreater(m.wav_info(self.positive)['duration_s'],0)
        with wave.open(str(self.negative),'wb') as w:
            w.setparams((1,2,8000,0,'NONE','not compressed'));w.writeframes(b'\x01\x01'*128001)
        with self.assertRaisesRegex(ValueError,'0.1..15'):m.wav_info(self.negative)

    def test_recovered_queue_drops_are_explicit_warning_not_zero_drop_pass(self):
        self.hw.drop_on_wake=True
        m.run(self.args,self.report)
        self.assertTrue(self.report['ok'])
        self.assertEqual(self.report['queue_drops_delta'],8)
        self.assertIn('zero-drop acceptance NOT passed',self.report['warnings'][0])


if __name__=='__main__':unittest.main()
