"""Voice acceptance guards, with no serial access or actual sound output."""
import argparse
from contextlib import nullcontext
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
import voice_acceptance as voice


class FakeLink:
    def __init__(self,stream,report):
        self.report=report
        self.volumes=[]
        self.demo_requests=0
        self.fail_navigation=False
        self.original=100

    def audio(self):return {'volume':str(self.original)}

    def page(self,name):
        if self.fail_navigation:raise RuntimeError('lost navigation reply')
        assert name=='eyes'

    def volume(self,percent):
        self.volumes.append(percent)
        return {'percent':percent}

    def wait(self,prefix):
        assert prefix=='MEO:VOICE_START'
        return {'accepted':'1'}

    def request(self,command,prefix):
        if command=='AI VOICE DEMO':
            self.demo_requests+=1
            return {'queued':'0' if self.demo_requests==2 else '1'}
        if command=='AI CANCEL':return {'queued':'1'}
        if command=='UI':return {'page':'eyes','eye_auto':'1'}
        raise AssertionError(command)


class VoiceAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.report={'transcript':[],'visual':'PENDING_USER','subjective_audio':'PENDING_USER'}
        self.link=FakeLink(None,self.report)

    def fake_idle(self,link,timeout=12):
        if link.demo_requests==3:return {'generation':'2','busy':'0','error':'259'}
        for sentence in (1,2):
            for cue in range(3):
                self.report['transcript'].append({'rx':
                    f'MEO:VOICE busy=1 generation=1 sentence={sentence} cue={cue} underruns=0 error=0'})
            self.report['transcript'].append({'rx':
                f'MEO:VOICE_SENTENCE_DONE generation=1 sentence={sentence} played=96000 frames=96000 error=0'})
        return {'generation':'1','busy':'0','error':'0'}

    def run_fake(self,volume=None):
        application={'elf_sha':'test-sha','port':'native-usb-double','boot_id':'same-boot'}
        with patch.object(voice.usb,'image_identity',return_value={'elf_sha':'test-sha'}), \
             patch.object(voice.usb,'check_application',return_value=application), \
             patch.object(voice.usb,'open_native',return_value=nullcontext(None)), \
             patch.object(voice.usb,'read_fault_diagnostic',return_value={'valid':'0'}), \
             patch.object(voice,'Link',return_value=self.link), \
             patch.object(voice,'wait_idle',side_effect=self.fake_idle), \
             patch.object(voice.time,'sleep'):
            if volume is None:voice.run(Path('mock-build'),self.report)
            else:voice.run(Path('mock-build'),self.report,volume)

    def test_default_records_50_and_restores_without_claiming_physical_pass(self):
        self.run_fake()
        self.assertEqual(self.link.volumes,[50,100])
        self.assertEqual(self.report['test_volume'],50)
        self.assertEqual(len(self.report['playback']['completed']),2)
        self.assertEqual(len(self.report['playback']['cue_pairs']),6)
        self.assertEqual(self.report['visual'],'PENDING_USER')
        self.assertEqual(self.report['subjective_audio'],'PENDING_USER')

    def test_explicit_level_does_not_override_saved_mute_afterward(self):
        self.link.original=0
        self.run_fake(35)
        self.assertEqual(self.link.volumes,[35,0])
        self.assertEqual(self.report['test_volume'],35)

    def test_error_restores_original_even_before_playback(self):
        self.link.fail_navigation=True
        with self.assertRaisesRegex(RuntimeError,'navigation'):
            self.run_fake()
        self.assertEqual(self.link.volumes,[100])

    def test_invalid_level_rejected_before_device_access(self):
        for level in ('0','101','-1','abc','50.5',None):
            with self.subTest(level=level),patch.object(voice.usb,'image_identity') as probe:
                with self.assertRaises(argparse.ArgumentTypeError):
                    voice.run(Path('mock-build'),self.report,level)
                probe.assert_not_called()


if __name__=='__main__':unittest.main()
