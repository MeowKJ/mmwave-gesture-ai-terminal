"""No TTS weights/GPU/network/device: sentence contract and cross-language IDs."""
from copy import deepcopy
import json
from pathlib import Path
import re
import sys
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
from voice_gateway import validate_plan,compile_sentence,tts_emotion,EXPRESSIONS

class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.plan=json.loads((ROOT/'tools/voice_plan.example.json').read_text(encoding='utf-8'))

    def test_every_sentence(self):
        self.assertIs(validate_plan(self.plan),self.plan)
        for chain in ([],None):
            bad=deepcopy(self.plan);bad['sentences'][1]['expressions']=chain
            with self.assertRaises(ValueError):validate_plan(bad)
        self.plan['sentences'][0]['text']='你好！你好吗？'
        with self.assertRaises(ValueError):validate_plan(self.plan)
        self.plan['sentences'][0]['text']='Hello. How are you?'
        with self.assertRaises(ValueError):validate_plan(self.plan)

    def test_invalid_inputs(self):
        for key,value in [('intensity',float('nan')),('intensity',True),('expression','REBOOT'),
                          ('at',.1),('at',True),('transition_ms',2001)]:
            bad=deepcopy(self.plan);bad['sentences'][0]['expressions'][0][key]=value
            with self.assertRaises(ValueError):validate_plan(bad)
        self.plan['generation']=True
        with self.assertRaises(ValueError):validate_plan(self.plan)

    def test_compilation_and_voice_axes(self):
        for i,s in enumerate(self.plan['sentences'],1):
            p=compile_sentence(3,i,s,111823)
            self.assertEqual(p['expressions'][0]['at_ms'],0)
            self.assertEqual(p['generation'],3)
            self.assertEqual((p['channels'],p['format']),(1,'pcm_s16le'))
            for j,c in enumerate(p['expressions']):
                end=p['expressions'][j+1]['at_ms']*48 if j+1<len(p['expressions']) else p['frames']
                self.assertLessEqual((c['at_ms']+c['transition_ms'])*48,end)
            self.assertEqual(len(tts_emotion(s)),8)
            self.assertLessEqual(sum(tts_emotion(s)),.65)

    def test_all_ids_match_firmware(self):
        source=(ROOT/'firmware/p4_gesture/main/meoave_model.c').read_text(encoding='utf-8')
        names=source[source.index('const char *meo_expression_name'):source.index('void meo_face_blend')]
        self.assertEqual(re.findall(r'"([A-Z]+)"',names)[:-1],list(EXPRESSIONS))
        self.assertGreaterEqual(len(EXPRESSIONS),16)

if __name__=='__main__':unittest.main()
