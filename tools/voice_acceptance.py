#!/usr/bin/env python3
"""Local sentence/face playback acceptance, native USB ONLY; no C5/reset/flash.

Uses the optional two-sentence speech fixture, not live ASR/LLM/IndexTTS.
Temporarily selects an explicit volume (default 50%), restoring the original
setting even on failure. The first 30% run was inaudible to the user; 50% was
heard with matching face/mouth motion on the SAME running image, without reset.
This comparison does not establish the root cause of the initial silence.
Serial/DMA results do not certify sound quality or physical screen appearance.
"""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import time
from audio_acceptance import Link,fields,usb,CONFIG,PROJECT,meoave

def statuses(report,start):
    return [fields(t['rx'],'MEO:VOICE') for t in report['transcript'][start:]
            if t.get('rx','').startswith('MEO:VOICE ')]

def wait_idle(link,timeout=12):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        link.send('AI STATUS');state=link.wait('MEO:VOICE',timeout=1)
        if state.get('busy')=='0':return state
        time.sleep(.08)
    raise TimeoutError('voice playback did not return idle')

def playback_volume(value):
    try:percent=int(value)
    except (TypeError,ValueError):
        raise argparse.ArgumentTypeError('voice listening test volume must be 1..100') from None
    if not 1<=percent<=100:
        raise argparse.ArgumentTypeError('voice listening test volume must be 1..100')
    return percent

def run(build,report,volume=50):
    volume=playback_volume(volume)
    report['test_volume']=volume
    expected=usb.image_identity(build);before=usb.check_application(expected['elf_sha'],10)
    report.update(image=expected,before=before)
    with usb.open_native(before['port'],usb.APP_PIDS) as stream:
        link=Link(stream,report);original=int(link.audio()['volume'])
        report['original_volume']=original
        try:
            link.page('eyes');link.volume(volume)
            start=len(report['transcript'])
            ack=link.request('AI VOICE DEMO','MEO:VOICE_DEMO')
            if ack.get('queued')!='1':raise RuntimeError('fixture missing or output busy')
            accepted=link.wait('MEO:VOICE_START')
            if accepted.get('accepted')!='1':raise RuntimeError('voice worker did not start')
            duplicate=link.request('AI VOICE DEMO','MEO:VOICE_DEMO')
            if duplicate.get('queued')!='0':raise RuntimeError('duplicate voice demo accepted')
            final=wait_idle(link)
            done=[fields(t['rx'],'MEO:VOICE_SENTENCE_DONE') for t in report['transcript'][start:]
                  if t.get('rx','').startswith('MEO:VOICE_SENTENCE_DONE ')]
            observed=statuses(report,start)
            if len(done)!=2 or [d['sentence'] for d in done]!=['1','2']:
                raise RuntimeError('two distinct sentence completions not observed')
            if any(d['played']!=d['frames'] or d['error']!='0' or d['generation']!=final['generation'] for d in done):
                raise RuntimeError('PCM completion / generation mismatch')
            cues={(d['sentence'],d['cue']) for d in observed if d.get('busy')=='1'}
            if not {(str(s),str(c)) for s in (1,2) for c in (0,1,2)}<=cues:
                raise RuntimeError('not all six sentence expression cues observed')
            if final['error']!='0' or any(d['underruns']!='0' for d in observed):
                raise RuntimeError('voice error or DMA underrun observed')
            report['playback']={'completed':done,'cue_pairs':sorted(cues),'final':final}
            # An actual second turn canceled after audio starts, not just a
            # fake timer-only state. Do not retry the mutating start/cancel.
            ack=link.request('AI VOICE DEMO','MEO:VOICE_DEMO')
            if ack.get('queued')!='1':raise RuntimeError('second generation rejected')
            link.wait('MEO:VOICE_START')
            time.sleep(.4)
            cancel=link.request('AI CANCEL','MEO:AI_CANCEL')
            if cancel.get('queued')!='1':raise RuntimeError('cancel rejected')
            canceled=wait_idle(link,3)
            if int(canceled['generation'])!=int(final['generation'])+1 or canceled['error']=='0':
                raise RuntimeError('canceled generation was not sealed')
            report['cancel']=canceled
            deadline=time.monotonic()+2
            while time.monotonic()<deadline:
                ui=link.request('UI','MEO:UI')
                if ui.get('page')=='eyes' and ui.get('eye_auto')=='1':break
                time.sleep(.05)
            else:raise RuntimeError('UI did not release sentence expression ownership')
            report['home']=ui
        finally:
            report['restored_volume']=link.volume(original)
    after=usb.check_application(expected['elf_sha'],5);report['after']=after
    if before['boot_id']!=after['boot_id']:raise RuntimeError('unexpected P4 reset')
    report['fault']=usb.read_fault_diagnostic(after['port'])
    if report['fault'].get('valid')!='0':raise RuntimeError('P4 fault present or diagnostic unavailable')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=PROJECT/meoave.cfg(CONFIG,'build','product_dir','build_meoave'))
    parser.add_argument('--report',required=True,type=Path)
    parser.add_argument('--volume',type=playback_volume,default=50,
                        help='temporary listening level 1..100 (default: 50); restores original volume')
    args=parser.parse_args()
    if args.report.exists():parser.error('choose a new report path; preserve prior evidence')
    report=dict(ok=False,started=datetime.now().astimezone().isoformat(),transcript=[],
                scope='P4 native USB local speech fixture only; NO C5/reset/flash/network AI',
                visual='PENDING_USER',subjective_audio='PENDING_USER',tts='Windows SAPI fixture, NOT IndexTTS2')
    try:
        run(args.build_dir,report,args.volume);report['ok']=True
        print('PASS: two PCM sentence completions, six expression cues, duplicate guard, cancel, same boot; visual/audio pending')
        return 0
    except Exception as e:
        report['error']=f'{type(e).__name__}: {e}';print(report['error'],file=sys.stderr);return 1
    finally:
        report['finished']=datetime.now().astimezone().isoformat();usb.write_report(args.report,report)

if __name__=='__main__':sys.exit(main())
