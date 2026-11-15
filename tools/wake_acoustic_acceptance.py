#!/usr/bin/env python3
"""Hands-free physical PC speaker -> P4 microphone -> ESP-SR acceptance.

No text wake injection, hardware button, C5 access or hidden recovery retries.
Uses offline Chinese Windows SAPI (or supplied PCM WAVs), leaves host volume
unchanged, observes USB while playing, then checks quiet/negative controls and
continuous inference. LVGL telemetry is NOT a physical-screen/camera inspection.
"""
from __future__ import annotations
import argparse
import base64
from datetime import datetime
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
import wave

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'firmware' / 'p4_gesture'
sys.path.insert(0, str(PROJECT))
import flash_usb as usb


def stamp():
    return datetime.now().astimezone().isoformat()


def save_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def fields(line, prefix):
    if not line.startswith(prefix + ' '):
        raise ValueError('Unexpected reply: ' + line)
    result = {}
    for item in line[len(prefix)+1:].split():
        if '=' not in item:
            raise ValueError('Malformed status field: ' + item)
        key, value = item.split('=', 1)
        if key in result:
            raise ValueError('Duplicate reply field: ' + key)
        result[key] = value
    return result


def make_sapi_wav(path, phrase, voice=None, rate=0):
    if os.name != 'nt':
        raise RuntimeError('Windows SAPI unavailable; supply --wav and --negative-wav')
    payload = base64.b64encode(json.dumps({'path': str(Path(path).resolve()), 'phrase': phrase,
        'voice': voice, 'rate': rate}, ensure_ascii=False).encode('utf-8')).decode('ascii')
    # JSON is DATA, never interpolated as PowerShell syntax (quotes, $, `, paths).
    script = r'''
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
Add-Type -AssemblyName System.Speech
$p=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__PAYLOAD__')) | ConvertFrom-Json
$s=New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
  $voices=@($s.GetInstalledVoices() | Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -like 'zh-*' })
  if ($p.voice) { $voices=@($voices | Where-Object { $_.VoiceInfo.Name -eq $p.voice }) }
  if ($voices.Count -eq 0) { throw 'No enabled Chinese SAPI voice; supply recorded WAV fixtures' }
  $s.SelectVoice($voices[0].VoiceInfo.Name)
  $s.Rate=[int]$p.rate; $s.Volume=100
  $s.SetOutputToWaveFile($p.path); $s.Speak($p.phrase)
  @{voice=$s.Voice.Name;culture=$s.Voice.Culture.Name;engine='Windows SAPI offline'} | ConvertTo-Json -Compress
} finally { $s.Dispose() }
'''.replace('__PAYLOAD__', payload)
    encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
        capture_output=True, encoding='utf-8', errors='replace', timeout=45, check=False)
    if result.returncode or not path.is_file():
        raise RuntimeError('Chinese SAPI failed: ' + (result.stderr or result.stdout).strip())
    return json.loads(result.stdout.lstrip('\ufeff').strip())


def wav_info(path):
    if not path.is_file() or not 44 <= path.stat().st_size <= 5_000_000:
        raise ValueError('WAV missing or exceeds 5 MB: ' + str(path))
    with wave.open(str(path), 'rb') as source:
        channels, width, rate, frames = (source.getnchannels(), source.getsampwidth(),
                                        source.getframerate(), source.getnframes())
        if channels not in (1, 2) or width != 2 or not 8000 <= rate <= 96000:
            raise ValueError('Need mono/stereo uncompressed 16-bit WAV')
        duration = frames / rate
        if not .1 <= duration <= 15:
            raise ValueError('WAV must last 0.1..15 seconds')
        data = source.readframes(frames)
        if len(data) != frames * channels * width or not any(data):
            raise ValueError('WAV empty, silent or truncated')
    import hashlib
    return {'path': str(path), 'duration_s': duration, 'rate': rate, 'channels': channels,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


class Speaker:
    """Async playback: the main thread must keep sampling during the utterance."""
    def start(self, path):
        import winsound
        winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_NODEFAULT | winsound.SND_ASYNC)
    def stop(self):
        import winsound
        winsound.PlaySound(None, 0)


class Link:
    def __init__(self, stream, report):
        self.stream, self.report = stream, report
        self.pending = b''
        self.events = []
        self.started = time.monotonic()

    def log(self, **item):
        transcript = self.report.setdefault('transcript', [])
        if len(transcript) >= 50000:
            raise RuntimeError('USB transcript limit exceeded')
        transcript.append({'t': round(time.monotonic()-self.started, 4), **item})

    def send(self, command):
        if not command.isascii() or '\n' in command or '\r' in command:
            raise ValueError('One ASCII command required')
        # Whitelist the only mutation: navigation, once. No text wake/reset commands.
        if command != 'UI' and command != 'UI GO eyes' and not re.fullmatch(r'MIC STATUS [a-f0-9]+', command):
            raise ValueError('Command is outside acoustic test scope')
        self.log(tx=command)
        self.stream.write((command+'\n').encode('ascii')); self.stream.flush()

    def readline(self, deadline):
        while time.monotonic() < deadline:
            if b'\n' in self.pending:
                raw, self.pending = self.pending.split(b'\n', 1)
                line = raw.decode('utf-8', errors='replace').strip()
                if not line:
                    continue
                self.log(rx=line)
                if any(word in line for word in ('Guru Meditation', "panic'ed", 'Stack smashing', 'assert failed')):
                    raise RuntimeError('P4 fault: '+line)
                if line.startswith('MEO:WAKE '):
                    self.events.append({'t': round(time.monotonic()-self.started,4), **fields(line,'MEO:WAKE')})
                return line
            data = self.stream.read(4096)
            self.pending += data
            if len(self.pending) > 16384:
                raise RuntimeError('Oversized/unterminated USB record')
        return None

    def status(self, timeout=2):
        self.send('UI')
        state = {'t': round(time.monotonic()-self.started,4)}
        prefixes = {'MEO:UI': 'ui', 'MEO:UI_AUDIO':'audio', 'MEO:UI_CAPTURE':'capture', 'MEO:WAKE_STATUS':'wake'}
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            line = self.readline(deadline)
            if not line:
                continue
            prefix = line.split(' ',1)[0]
            if prefix in prefixes:
                key = prefixes[prefix]
                if key in state:
                    raise RuntimeError('Overlapping/stale UI reply')
                state[key] = fields(line,prefix)
                if key == 'wake':
                    if not all(k in state for k in ('ui','audio','capture')):
                        raise RuntimeError('Incomplete UI status; update the selected P4 image')
                    validate_state(state)
                    return state
        raise TimeoutError('No complete UI/wake status')

    def microphone(self):
        token=secrets.token_hex(6)
        self.send('MIC STATUS '+token)
        deadline=time.monotonic()+2
        while time.monotonic()<deadline:
            line=self.readline(deadline)
            if line and line.startswith('MEO:MIC '):
                result=fields(line,'MEO:MIC')
                if result.get('token')==token:
                    return result
        raise TimeoutError('No fresh microphone level')


def ready(wake):
    return all(wake.get(k)=='1' for k in ('linked','loaded','keyword_ready','requested','armed')) and wake.get('error')=='NONE'


def validate_state(state):
    w=state['wake']
    for key in ('acoustic','text_tests','chunks','drops','resets'):
        if not w.get(key,'').isdigit():
            raise ValueError('Missing/invalid wake counter: '+key)
    if w.get('guarded')!='0' or w.get('error') not in ('NONE','MODEL_LOADING','MODEL_REBUILDING'):
        raise RuntimeError('ESP-SR runtime fault: '+str(w))


def wait_ready(link, seconds):
    deadline=time.monotonic()+seconds
    last=None
    while time.monotonic()<deadline:
        last=link.status()
        if ready(last['wake']) and last['audio'].get('mic_running')=='1' and last['ui'].get('page')=='eyes':
            return last
        time.sleep(.15)
    raise TimeoutError('ESP-SR did not become armed: '+str(last))


def check_sample(state, baseline, tracker):
    w=state['wake']
    if state['ui'].get('page')!='eyes':
        raise RuntimeError('Device left the AI page')
    if w['text_tests']!=baseline['wake']['text_tests']:
        raise RuntimeError('Text injection occurred; not an acoustic test')
    for key in ('chunks','acoustic','resets','drops'):
        if int(w[key])<int(baseline['wake'][key]):
            raise RuntimeError('Counter regressed/reset: '+key)
    if state['audio'].get('mic_running')!='1':
        raise RuntimeError('Microphone capture is not active')
    if not ready(w):
        if w.get('error')!='MODEL_REBUILDING':
            raise RuntimeError('ESP-SR lost readiness: '+str(w))
        tracker.setdefault('rebuilding_since',time.monotonic())
        if time.monotonic()-tracker['rebuilding_since']>3:
            raise RuntimeError('Model rebuilding exceeded 3 seconds')
    else:
        tracker.pop('rebuilding_since',None)
    if int(w['chunks'])>tracker.get('chunks',-1):
        tracker['chunks']=int(w['chunks']); tracker['progress_at']=time.monotonic()
    elif time.monotonic()-tracker.get('progress_at',time.monotonic())>3:
        raise RuntimeError('Inference stalled for over 3 seconds')


def observe(link, seconds, baseline, record, playback=None, speaker=None):
    record.update(samples=[],events=[],started=stamp())
    start=time.monotonic(); event_start=len(link.events)
    tracker={}
    record['mic_before']=link.microphone()
    try:
        if playback:
            record['playback_t']=round(time.monotonic()-link.started,4)
            speaker.start(playback)
        record['mic_samples']=[]
        next_mic=start
        while time.monotonic()-start<seconds:
            state=link.status()
            record['samples'].append(state)
            check_sample(state,baseline,tracker)
            if playback and time.monotonic()>=next_mic:
                record['mic_samples'].append({'t':round(time.monotonic()-link.started,4),
                                             **link.microphone()})
                next_mic=time.monotonic()+.5
            time.sleep(.12)
        # Final bounded readiness: never accept a transient as final success.
        state=wait_ready(link,3)
        record['samples'].append(state);check_sample(state,baseline,tracker)
        record['mic_after']=link.microphone()
    finally:
        if playback:
            speaker.stop()
        record['events']=link.events[event_start:]
        record['elapsed_s']=round(time.monotonic()-start,3)
        record['finished']=stamp()
    return record['samples'][-1]


def assess(record, baseline, positive):
    samples=record.get('samples',[])
    if not samples:
        return ['no observations']
    errors=[]
    count=int(samples[-1]['wake']['acoustic'])-int(baseline['wake']['acoustic'])
    record['detections']=count
    if int(samples[-1]['wake']['chunks'])<=int(baseline['wake']['chunks']):
        errors.append('no inference progress')
    if not positive:
        if count or record['events']:
            errors.append('false wake in quiet/negative control')
        if record.get('kind') in ('quiet','cooldown') and samples[-1]['capture'].get('visible')!='0':
            errors.append('listening indicator did not hide in silence')
        return errors
    if count!=1:
        errors.append(f'expected exactly one acoustic detection, got {count}')
    events=record['events']
    if len(events)!=1 or any(e.get('source')!='ACOUSTIC_ESP_SR' or e.get('detected')!='1' for e in events):
        errors.append('missing/unexpected acoustic event')
    if events and int(events[-1].get('count','-1'))!=int(samples[-1]['audio'].get('wake_detections','-2')):
        errors.append('event/UI generation mismatch')
    if not any(s['audio'].get('wake_detected')=='1' and s['ui'].get('eye')=='2' for s in samples):
        # CURIOUS enum is checked against source below by tests; no text injection.
        errors.append('wake acknowledgement/curious eyes not observed together')
    if not any(s['capture'].get('visible')=='1' and int(s['capture'].get('fill_h','0'))>0 for s in samples):
        errors.append('input-driven microphone fill not observed during speech')
    if int(samples[-1]['capture'].get('seq','0'))<=int(baseline['capture'].get('seq','0')):
        errors.append('microphone fill did not respond')
    if samples[-1]['capture'].get('visible')!='0':
        errors.append('microphone mark did not hide after speech')
    return errors


def run(args, report):
    report.update(scope='physical PC speaker -> P4 microphone -> ESP-SR; no text injection',
        visual='LVGL object telemetry + separate desktop tests; physical screen NOT camera-verified',
        host_volume='unchanged',attempts=[],controls=[],transcript=[],stage='prepare')
    save_report(args.report,report)
    build=Path(args.build_dir).resolve()
    identity=usb.image_identity(build);report['image']=identity
    fixtures=Path(args.report).with_suffix('.audio')
    fixtures.mkdir(parents=True,exist_ok=False)
    for key,provided,text in (('positive',args.wav,args.phrase),('negative',args.negative_wav,args.negative_phrase)):
        path=Path(provided).resolve() if provided else fixtures/(key+'.wav')
        info={'origin':'user WAV; transcript not independently verified'}
        if not provided:
            info=make_sapi_wav(path,text,args.voice,args.rate)
        report[key]={**info,**wav_info(path),'phrase':text}
    save_report(args.report,report)
    # Validate exact native target BEFORE any firmware/USB mutation.
    if args.port:
        usb.select_port(usb.APP_PIDS | (usb.ROM_PIDS if args.flash else set()),args.port)
    if args.flash:
        report['stage']='flash';report['flash']={};save_report(args.report,report)
        try:
            usb.flash_once(build,args.port,baud=args.baud,app_timeout=args.timeout,report=report['flash'])
        finally:
            save_report(args.report,report)
    before=usb.check_application(identity['elf_sha'],timeout=args.timeout)
    if args.port and not args.flash and before['port'].casefold()!=args.port.casefold():
        raise RuntimeError('Application port changed; refusing another target')
    report['before']=before;report['stage']='ready';save_report(args.report,report)
    speaker=Speaker()
    with usb.open_native(before['port'],usb.APP_PIDS) as stream:
        link=Link(stream,report)
        link.send('UI GO eyes')
        initial=wait_ready(link,args.timeout)
        report['initial']=initial
        failures=[]
        baseline=initial
        for kind in ('quiet','negative'):
            record={'kind':kind};report['controls'].append(record)
            path=Path(report['negative']['path']) if kind=='negative' else None
            seconds=(report['negative']['duration_s']+args.listen_after) if path else args.gap
            previous=baseline
            baseline=observe(link,seconds,baseline,record,path,speaker)
            record['errors']=assess(record,previous,False)
            failures.extend(record['errors']);save_report(args.report,report)
        for index in range(args.repeat):
            cooldown={'kind':'cooldown'};report['controls'].append(cooldown)
            previous=baseline
            baseline=observe(link,args.gap,baseline,cooldown)
            cooldown['errors']=assess(cooldown,previous,False)
            failures.extend(cooldown['errors'])
            attempt={'number':index+1};report['attempts'].append(attempt)
            previous=baseline
            baseline=observe(link,report['positive']['duration_s']+args.listen_after,baseline,
                attempt,Path(report['positive']['path']),speaker)
            attempt['errors']=assess(attempt,previous,True)
            failures.extend(attempt['errors'])
            print(f"Trial {index+1}/{args.repeat}: acoustic={attempt['detections']} errors={attempt['errors']}",flush=True)
            save_report(args.report,report)
        report['stage']='soak';report['soak']={'samples':[],'seconds':args.soak_seconds}
        save_report(args.report,report)
        soak_initial=baseline
        soak_started=time.monotonic()
        deadline=soak_started+args.soak_seconds
        tracker={}
        last_saved=time.monotonic()
        while time.monotonic()<deadline:
            baseline=link.status();report['soak']['samples'].append(baseline)
            check_sample(baseline,soak_initial,tracker)
            if int(baseline['wake']['acoustic'])!=int(soak_initial['wake']['acoustic']):
                raise RuntimeError('False wake during quiet soak')
            if time.monotonic()-last_saved>=10:
                save_report(args.report,report);last_saved=time.monotonic()
            time.sleep(.8)
        report['soak']['elapsed_s']=round(time.monotonic()-soak_started,3)
        report['final']=wait_ready(link,3)
        check_sample(report['final'],initial,{})
        if report['final']['capture'].get('ack')!='0':
            failures.append('wake acknowledgement did not return to listening')
        if report['final']['capture'].get('visible')!='0':
            failures.append('listening mark did not hide in final silence')
    report['after']=usb.check_application(identity['elf_sha'],timeout=5)
    if report['after']['boot_id']!=before['boot_id']:
        failures.append('unexpected reset/boot ID change')
    report['fault']=usb.read_fault_diagnostic(report['after']['port'])
    if report['fault'].get('valid')!='0':
        failures.append('P4 fault diagnostic not clean')
    report['failures']=failures
    report['successes']=sum(not a['errors'] for a in report['attempts'])
    report['stability_qualified']=args.soak_seconds>=420
    report['stability_scope']='selected-window inference continuity, not zero-drop or long-term reliability'
    report['queue_drops_delta']=int(report['final']['wake']['drops'])-int(initial['wake']['drops'])
    report['warnings']=[]
    if report['queue_drops_delta']:
        report['warnings'].append(f"{report['queue_drops_delta']} microphone queue blocks dropped; "
                                  'stream recovery observed, zero-drop acceptance NOT passed')
    if failures:
        raise RuntimeError('; '.join(dict.fromkeys(failures)))
    report.update(ok=True,stage='complete')


def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,default=PROJECT/'build_meoave')
    p.add_argument('--port');p.add_argument('--baud',default='921600')
    p.add_argument('--flash',action='store_true')
    p.add_argument('--wav',type=Path);p.add_argument('--negative-wav',type=Path)
    p.add_argument('--phrase',default='Hi，喵喵。')
    p.add_argument('--negative-phrase',default='今天天气真好，我们一起听音乐。')
    p.add_argument('--voice');p.add_argument('--rate',type=int,default=0)
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--gap',type=float,default=3)
    p.add_argument('--listen-after',type=float,default=3)
    p.add_argument('--timeout',type=float,default=60)
    p.add_argument('--soak-seconds',type=int,default=420)
    p.add_argument('--report',type=Path,required=True)
    return p


def main(argv=None):
    p=parser();args=p.parse_args(argv)
    if not 1<=args.repeat<=10 or not 2<=args.gap<=15 or not .5<=args.listen_after<=10:
        p.error('repeat 1..10, gap 2..15, listen-after 0.5..10')
    if not -10<=args.rate<=10 or not 3<=args.timeout<=120 or not 0<=args.soak_seconds<=1800:
        p.error('rate -10..10, timeout 3..120, soak-seconds 0..1800')
    if args.report.exists():
        p.error('Report exists; preserve evidence and choose a new path')
    report={'ok':False,'started':stamp()}
    try:
        run(args,report)
        print(f"PASS: {report['successes']}/{args.repeat} real acoustic trials; controls and {args.soak_seconds}s soak; no reset; "
              + ('7-minute runtime continuity checked' if report['stability_qualified'] else 'SHORT RUN, stability not qualified'))
        for warning in report['warnings']:
            print('WARNING: '+warning)
        return 0
    except Exception as exc:
        report['error']=f'{type(exc).__name__}: {exc}'
        try:
            port=usb.app_port()
            if port: report['fault_after_failure']=usb.read_fault_diagnostic(port.device)
        except Exception as diagnostic:
            report['diagnostic_error']=str(diagnostic)
        print('FAIL: '+report['error'],file=sys.stderr)
        return 1
    finally:
        report['finished']=stamp();save_report(args.report,report)


if __name__=='__main__':
    raise SystemExit(main())
