#!/usr/bin/env python3
"""Sentence compiler / local IndexTTS2 adapter, NOT a deployed WSS/ASR/LLM server.

validate needs only stdlib. synthesize must run in the separately installed
upstream IndexTTS environment. No pip/model downloads, API keys or GPIO control.
"""
from __future__ import annotations
import argparse
import hashlib
import inspect
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import tomllib

EXPRESSIONS = ('CALM','HAPPY','CURIOUS','SLEEPY','SURPRISED','WINK','SAD',
               'EXCITED','SHY','LOVE','PROUD','CONFUSED','WORRIED','ANGRY','FOCUSED','RELIEVED')
RATE=48000

def uint(n, maximum=0xFFFFFFFF):
    return type(n) is int and 0<n<=maximum

def unit(n):
    return type(n) in (int,float) and math.isfinite(n) and 0<=n<=1

def validate_plan(plan):
    if not isinstance(plan,dict) or not uint(plan.get('generation')):
        raise ValueError('generation must be a positive uint32')
    sentences=plan.get('sentences')
    if not isinstance(sentences,list) or not 1<=len(sentences)<=64:
        raise ValueError('need 1..64 sentences')
    for s in sentences:
        if not isinstance(s,dict):raise ValueError('sentence must be an object')
        text=s.get('text')
        if not isinstance(text,str) or not text.strip() or '\0' in text or len(text.encode('utf-8'))>768:
            raise ValueError('nonempty UTF-8 text required; maximum 768 bytes')
        # Do not accept one mood on an entire multi-sentence paragraph. A short
        # sentence/clause without a final punctuation mark remains legitimate.
        parts=[x.strip(' \r\n\t”’\"\'') for x in re.split(r'(?<=[。！？!?])|(?<=\.)\s+|\n+',text)]
        if len([x for x in parts if x])>1:raise ValueError('split text: each sentence needs its OWN chain')
        chain=s.get('expressions')
        if not isinstance(chain,list) or not 1<=len(chain)<=32:
            raise ValueError('EVERY sentence requires 1..32 expression cues')
        prev=-1.
        for c in chain:
            if not isinstance(c,dict):raise ValueError('cue must be an object')
            at=c.get('at')
            if not unit(at) or at>=1 or at<=prev or (prev<0 and at!=0):
                raise ValueError('cue fractions must start at zero and strictly increase below one')
            if c.get('expression') not in EXPRESSIONS or not unit(c.get('intensity')):
                raise ValueError('unknown expression or intensity outside 0..1')
            transition=c.get('transition_ms',180)
            if type(transition) is not int or not 0<=transition<=2000:
                raise ValueError('transition_ms must be integer 0..2000')
            prev=at
    return plan

def compile_sentence(generation, sentence_id, sentence, frames):
    validate_plan(dict(generation=generation,sentences=[sentence]))
    if not uint(sentence_id) or not uint(frames,RATE*30):raise ValueError('invalid sentence ID/PCM frame count')
    cues=[]
    for c in sentence['expressions']:
        at=int(c['at']*frames/48)
        if cues and at<=cues[-1]['at_ms']:raise ValueError('audio too short for distinct cue times')
        if at*48>=frames:raise ValueError('cue after audio end')
        cues.append(dict(at_ms=at,expression=c['expression'],intensity=c['intensity'],
                         transition_ms=c.get('transition_ms',180)))
    for i,c in enumerate(cues):
        end=cues[i+1]['at_ms'] if i+1<len(cues) else frames//48
        c['transition_ms']=min(c['transition_ms'],end-c['at_ms'])
    return dict(v=1,generation=generation,sentence_id=sentence_id,sample_rate=RATE,
                channels=1,format='pcm_s16le',frames=frames,text=sentence['text'],
                timing_method='proportional_not_word_aligned',expressions=cues)

def tts_emotion(sentence):
    # Face vocabulary deliberately differs from IndexTTS2's eight voice axes:
    # happy, angry, sad, afraid, disgusted, melancholic, surprised, calm.
    # Choose one duration-weighted sentence mood, not a claim of within-word
    # voice emotion control. The device still performs EVERY facial cue.
    indices={'HAPPY':0,'EXCITED':0,'LOVE':0,'PROUD':0,'WINK':0,'SHY':0,
             'ANGRY':1,'SAD':2,'WORRIED':3,'SURPRISED':6,'CURIOUS':6,'CONFUSED':6}
    scores=[0.]*8;chain=sentence['expressions']
    for i,c in enumerate(chain):
        length=(chain[i+1]['at'] if i+1<len(chain) else 1)-c['at']
        scores[indices.get(c['expression'],7)]+=length*c['intensity']*.65
    return scores

class IndexTTS2Backend:
    def __init__(self, config):
        # Paths and revision are explicit local configuration, never guesses.
        def path(key, directory=False):
            raw=config.get(key,'')
            if not raw:raise ValueError(f'configure voice.{key} first')
            p=Path(raw).resolve()
            if not (p.is_dir() if directory else p.is_file()):raise ValueError(f'missing voice.{key}')
            return p
        repo=path('repo',True);weights=path('model_dir',True);cfg=path('cfg_path');self.reference=path('reference_wav')
        revision=config.get('revision','')
        actual=subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()
        if not re.fullmatch('[0-9a-f]{40}',revision) or actual!=revision:
            raise ValueError('pin voice.revision to the reviewed IndexTTS repository HEAD (40-char SHA)')
        aux=config.get('aux_paths',{})
        for key in ('w2v_bert','semantic_codec','campplus','bigvgan'):
            if not aux.get(key) or not Path(aux[key]).exists():raise ValueError(f'local voice.aux_paths.{key} required')
        # No fallback downloads if an asset is absent. Upstream current v2
        # exposes local aux_paths; older revisions must be adapted explicitly.
        os.environ['HF_HUB_OFFLINE']='1';os.environ['TRANSFORMERS_OFFLINE']='1'
        sys.path.insert(0,str(repo))
        from indextts.infer_v2 import IndexTTS2
        if 'aux_paths' not in inspect.signature(IndexTTS2).parameters:
            raise ValueError('this upstream revision lacks the reviewed offline aux_paths API')
        self.tts=IndexTTS2(cfg_path=str(cfg),model_dir=str(weights),device=config.get('device','cuda:0'),
            use_fp16=True,use_cuda_kernel=False,use_deepspeed=False,use_qwen_emo=False,
            aux_paths={k:str(Path(v).resolve()) for k,v in aux.items()})
        self.revision=actual

    def synthesize(self, sentence, path):
        self.tts.infer(spk_audio_prompt=str(self.reference),text=sentence['text'],output_path=str(path),
            emo_vector=tts_emotion(sentence),use_random=False,use_emo_text=False,stream_return=False)

def normalized_pcm(path):
    # Imports only in the separate TTS environment. Proper low-pass resampling,
    # never sample dropping; preserve hardware's validated 48k clock.
    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly
    audio,rate=sf.read(str(path),dtype='float32',always_2d=True)
    if audio.shape[1]!=1 or not 0<len(audio)<=rate*30 or not np.isfinite(audio).all():
        raise ValueError('TTS must return finite mono audio, 0..30 seconds')
    samples=audio[:,0]
    if rate!=RATE:
        divisor=math.gcd(rate,RATE);samples=resample_poly(samples,RATE//divisor,rate//divisor)
    pcm=np.rint(np.clip(samples,-1,1)*32767).astype('<i2')
    return pcm.tobytes(),len(pcm)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['validate','synthesize'])
    p.add_argument('--plan',required=True,type=Path)
    p.add_argument('--config',type=Path,default=Path(__file__).with_name('config.toml'))
    p.add_argument('--out',type=Path)
    args=p.parse_args()
    try:
        if args.plan.stat().st_size>65536:raise ValueError('plan exceeds 64KiB')
        plan=validate_plan(json.loads(args.plan.read_text(encoding='utf-8')))
        if args.action=='validate':print(f"valid: {len(plan['sentences'])} sentences, all with expression chains");return
        if not args.out or args.out.exists():raise ValueError('--out must name a NEW directory')
        config=tomllib.loads(args.config.read_text(encoding='utf-8')).get('voice',{})
        if config.get('engine')!='indextts2':raise ValueError('only the reviewed indextts2 adapter is implemented')
        start=time.perf_counter();backend=IndexTTS2Backend(config);load_ms=(time.perf_counter()-start)*1000
        args.out.mkdir(parents=True)
        report=dict(engine='IndexTTS2',revision=backend.revision,model_load_ms=load_ms,
                    mode='whole_sentence_not_subsentence_streaming',sentences=[])
        for i,s in enumerate(plan['sentences'],1):
            wav=args.out/f'{i:03}.wav';start=time.perf_counter();backend.synthesize(s,wav)
            synth_ms=(time.perf_counter()-start)*1000
            pcm,frames=normalized_pcm(wav);packet=compile_sentence(plan['generation'],i,s,frames)
            (args.out/f'{i:03}.pcm').write_bytes(pcm)
            (args.out/f'{i:03}.json').write_text(json.dumps(packet,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            report['sentences'].append(dict(id=i,synthesis_ms=synth_ms,audio_ms=frames/48,
                rtf=synth_ms/(frames/48),pcm_sha256=hashlib.sha256(pcm).hexdigest(),
                device_playback_tested=False))
        (args.out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(args.out/'report.json')
    except (ValueError,OSError,ImportError,KeyError) as e:
        p.exit(2,f'voice: {e}\n')

if __name__=='__main__':main()
