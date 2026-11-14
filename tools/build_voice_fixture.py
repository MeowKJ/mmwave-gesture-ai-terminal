#!/usr/bin/env python3
"""Pack TWO explicitly supplied smoke-test WAVs, not TTS/model benchmarking.

Input phrases: 1) 你好，我在这里。  2) 今天也陪着你。
WAV format: 48kHz, signed 16-bit mono; each 1.8..30 seconds.
Artifacts stay in ignored build_ui_assets. No voice cloning or network calls.
"""
from pathlib import Path
import argparse
import hashlib
import json
import wave

ROOT = Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('wav',type=Path,nargs=2)
    p.add_argument('--provenance',required=True,help='Engine/voice used, not a fabricated IndexTTS claim')
    args=p.parse_args()
    blocks=[];frames=[]
    for path in args.wav:
        with wave.open(str(path),'rb') as f:
            if (f.getframerate(),f.getsampwidth(),f.getnchannels(),f.getcomptype())!=(48000,2,1,'NONE'):
                p.error('expected 48k/S16/mono/uncompressed WAV')
            if not 86400<=f.getnframes()<=1440000:
                p.error('each phrase must last 1.8..30 seconds')
            frames.append(f.getnframes());blocks.append(f.readframes(f.getnframes()))
    dest=ROOT/'firmware/p4_gesture/build_ui_assets/voice_fixture'
    dest.mkdir(parents=True,exist_ok=True)
    data=b''.join(blocks)
    (dest/'voice_fixture.pcm').write_bytes(data)
    (dest/'voice_fixture.h').write_text('#pragma once\n'+''.join(
        f'#define MEO_FIXTURE_FRAMES_{i+1} {n}u\n' for i,n in enumerate(frames)),encoding='utf-8')
    (dest/'provenance.json').write_text(json.dumps(dict(provenance=args.provenance,
        frames=frames,sha256=hashlib.sha256(data).hexdigest(),purpose='local sentence/PCM sync smoke test only'),
        ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(dest)

if __name__=='__main__':main()
