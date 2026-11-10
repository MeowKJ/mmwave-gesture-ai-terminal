#!/usr/bin/env python3
"""Build/stage an app-only C5 SDIO diagnostic and its exact backup-based restore.

prepare does not access hardware. install/flash only runs when requested.
run requires the user to have released ALL three download straps; it resets C5
once and performs bounded signature/heartbeat/random CMD53 echo checks.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import time

import meoave
import c5_program as program
import c5_inspect as inspection

ROOT=meoave.ROOT
RESULTS=ROOT/'firmware/p4_gesture/test-results'
STATUS=re.compile(r'MEO:C5_MINIMAL token=(?P<token>[a-zA-Z0-9]{1,16}) operation=(?P<operation>[a-zA-Z0-9]{1,16}) '
    r'state=(?P<state>IDLE|QUEUED|RUNNING|PASS|FAILED|OBSERVED) stage=(?P<stage>[A-Z0-9_]+) '
    r'error=(?P<error>\d+) reset_completed=(?P<reset_completed>[01]) samples=(?P<samples>\d+) echoes=(?P<echoes>\d+) '
    r'hb_first=(?P<hb_first>\d+) hb_last=(?P<hb_last>\d+) reason=(?P<reason>\d+) elf_sha=(?P<elf_sha>[0-9a-f]{64}|none)')
ACK=re.compile(r'MEO:C5_MINIMAL_ACCEPT token=([a-zA-Z0-9]{1,16}) accepted=([01])')
TRACE=re.compile(r'MEO:C5_TRACE token=([a-zA-Z0-9]{1,16}) operation=([a-zA-Z0-9]{1,16}) '
    r'index=(\d+) count=(\d+) bytes=(\d+) error=(\d+) uptime_ms=(\d+) raw=([0-9a-f]{24})')
RESET_TRACE=re.compile(r'MEO:C5_RESET_TRACE token=(?P<token>[a-zA-Z0-9]{1,16}) operation=(?P<operation>[a-zA-Z0-9]{1,16}) '
    r'valid=(?P<valid>[01]) error=(?P<error>\d+) before=(?P<before>-?\d+) low_begin=(?P<low_begin>-?\d+) '
    r'low_end=(?P<low_end>-?\d+) high_begin=(?P<high_begin>-?\d+) high_end=(?P<high_end>-?\d+) '
    r'low_us=(?P<low_us>\d+) ie=(?P<ie>[01]) oe=(?P<oe>[01]) od=(?P<od>[01]) '
    r'mux=(?P<mux>\d+) signal=(?P<signal>\d+) peripheral=(?P<peripheral>[01]) sleep=(?P<sleep>[01]) scope=P4_PAD_ONLY')


def parse_reset_trace(line):
    match=RESET_TRACE.fullmatch(line)
    if not match: raise ValueError('Malformed reset pad evidence')
    trace=match.groupdict()
    for key in trace:
        if key not in ('token','operation'): trace[key]=int(trace[key])
    for key in ('before','low_begin','low_end','high_begin','high_end'):
        if trace[key] not in (-1,0,1): raise ValueError('Impossible pad level')
    trace['pad_sequence_verified']=bool(trace['valid'] and not trace['error'] and
        trace['before']==1 and trace['low_begin']==0 and trace['low_end']==0 and
        trace['high_begin']==1 and trace['high_end']==1 and trace['low_us']>=1000 and
        trace['ie'] and trace['oe'] and not trace['od'] and trace['mux']==1 and trace['signal']==256)
    # P4 PIN_FUNC_GPIO=1 / SIG_GPIO_OUT_IDX=256: peripheral OE selector is
    # immaterial for this dedicated GPIO source (IDF gpio_hal_matrix_out_default).
    trace['scope']='P4_PAD_ONLY; C5 EN waveform/boot mode NOT_VERIFIED'
    return trace


def read_reset_trace(stream, operation, clock=time.monotonic):
    token=secrets.token_hex(6);pending=b'';end=clock()+4;poll=0
    while clock()<end:
        if clock()>=poll:
            stream.write(f'C5 RESETTRACE {token}\n'.encode());stream.flush();poll=clock()+.5
        pending+=stream.read(4096)
        while b'\n' in pending:
            raw,pending=pending.split(b'\n',1)
            line=raw.decode('ascii',errors='replace').rstrip('\r')
            match=RESET_TRACE.fullmatch(line)
            if not match or match['token']!=token or match['operation']!=operation: continue
            return parse_reset_trace(line)
        pending=pending[-8192:]
    raise TimeoutError('No fresh reset pad evidence; no reset retry')


def parse_status(line, expected_sha, observe=False):
    match=STATUS.fullmatch(line)
    if not match: raise ValueError('Malformed minimal diagnostic status')
    s=match.groupdict()
    for key in ('error','reset_completed','samples','echoes','hb_first','hb_last','reason'):
        s[key]=int(s[key])
    if s['samples']>20 or s['echoes']>16: raise ValueError('Impossible diagnostic counters')
    if not observe and s['elf_sha']!='none' and s['elf_sha']!=expected_sha:
        raise RuntimeError('C5 diagnostic ELF identity mismatch')
    if s['state']=='PASS' and (s['error'] or not s['reset_completed'] or s['samples']!=20 or
            s['echoes']!=16 or s['hb_last']<=s['hb_first'] or s['elf_sha']!=expected_sha):
        raise ValueError('Incomplete application execution/echo evidence')
    if s['state']=='OBSERVED' and (s['error'] or s['reset_completed'] or s['echoes']):
        raise ValueError('Passive capture must not reset C5 or send echo packets')
    return s


def exchange(stream, expected_sha, report, run=False, clock=time.monotonic, observe=False):
    token=secrets.token_hex(6)
    if run:
        command='DIAGOBSERVE' if observe else 'DIAGRUN'
        stream.write(f'C5 {command} {token}\n'.encode());stream.flush()
    end=clock()+(55 if run else 5);poll=0;pending=b'';last=None
    while clock()<end:
        if clock()>=poll:
            stream.write(f'C5 DIAGSTATUS {token}\n'.encode());stream.flush()
            poll=clock()+.5
        pending+=stream.read(4096)
        while b'\n' in pending:
            raw,pending=pending.split(b'\n',1)
            line=raw.decode('ascii',errors='replace').rstrip('\r')
            if len(report['lines'])<3000: report['lines'].append(line)
            ack=ACK.fullmatch(line)
            if ack and ack[1]==token and ack[2]=='0': raise RuntimeError('DIAGRUN rejected; no retry')
            if not line.startswith('MEO:C5_MINIMAL '): continue
            s=parse_status(line,expected_sha,observe=observe)
            if s['token']!=token or (run and s['operation']!=token): continue
            report['last_status']=s
            progress=(s['state'],s['stage'],s['samples'],s['echoes'])
            if progress!=last:
                print('C5 minimal:',*progress,flush=True);last=progress
            if s['state']=='FAILED': raise RuntimeError(f"C5 minimal failed at {s['stage']}: {s['error']}; no retry/reset")
            if not run or s['state']==('OBSERVED' if observe else 'PASS'): return s
        if len(pending)>8192: raise ValueError('Unbounded diagnostic output')
    raise TimeoutError('Minimal test did not finish; no retry/reset')


def read_traces(stream, operation, report, expected_count=None):
    records=[]
    for index in range(20):
        token=secrets.token_hex(6);pending=b'';end=time.monotonic()+3;poll=0
        found=None
        while time.monotonic()<end and found is None:
            if time.monotonic()>=poll:
                stream.write(f'C5 DIAGTRACE {token} {index}\n'.encode());stream.flush()
                poll=time.monotonic()+.5  # cache query only, never repeat capture
            pending+=stream.read(4096)
            while b'\n' in pending:
                raw,pending=pending.split(b'\n',1)
                line=raw.decode('ascii',errors='replace').rstrip('\r')
                m=TRACE.fullmatch(line)
                if not m or m[1]!=token or m[2]!=operation or int(m[3])!=index: continue
                count,size,error,ms=map(int,m.group(4,5,6,7))
                if count>20 or size>12: raise ValueError('Impossible trace bounds')
                found={'index':index,'count':count,'bytes':size,'error':error,'uptime_ms':ms,'raw':m[8]}
                break
            pending=pending[-8192:]
        if found is None: raise TimeoutError('Cached trace unavailable; no reset/recapture')
        if index>=found['count']: break
        records.append(found)
        report['traces']=records
        if len(records)==found['count']: break
    if expected_count is not None and len(records)!=expected_count:
        raise RuntimeError('Incomplete passive trace evidence')
    return records


def summarize_traces(records):
    successful=[r for r in records if r['bytes']==12 and r['error']==0]
    return {'captured':len(records),'complete_reads':len(successful),
        'all_zero':bool(records) and len(successful)==len(records) and all(r['raw']=='0'*24 for r in successful),
        'unique_raw':sorted({r['raw'] for r in successful}),
        'app_execution':'NOT_VERIFIED','echo':'NOT_EXECUTED'}


def backup_data(path):
    source=json.loads(path.read_text(encoding='utf-8'))
    if not all(source.get(k) for k in ('ok','completed','full_flash')):
        raise ValueError('Complete verified Flash backup required')
    data=Path(source['output']).read_bytes()
    if hashlib.sha256(data).hexdigest()!=source['sha256']:
        raise ValueError('Backup digest mismatch')
    info=inspection.inspect_backup(data)
    slot=next(p for p in info['partitions'] if p['name']=='ota_0')
    if slot['offset']!=0x20000 or slot['size']!=0x1e0000:
        raise ValueError('Unexpected ota_0 layout')
    if info['predicted_boot']['partition']!='ota_0' or not all(x['erased'] for x in info['otadata']):
        raise ValueError('Only the observed blank otadata / ota_0 fallback is supported')
    return data,source,info


def stage_payload(folder, app, mode, backup_sha):
    folder.mkdir(parents=True,exist_ok=True)
    (folder/'application.bin').write_bytes(app)
    payload=folder/'payload.bin'
    payload.write_bytes(b'\xff'*0x20000+app)
    manifest={'flash_files':{'0x20000':'application.bin'},
        'extra_esptool_args':{'chip':'esp32c5'},
        'meoave_app_only':{'mode':mode,'partition_size':0x1e0000,'backup_sha256':backup_sha}}
    (folder/'flasher_args.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    meoave.programmer_payload_layout(payload)
    return payload


def prepare(config,args):
    # Verify backup FIRST; compilation/generation must never overwrite the backup.
    data,source,old=backup_data(args.backup_report)
    project=ROOT/'firmware/c5_hosted/diagnostic'; build=project/'build_diag'
    env=meoave.environment(config)
    if not env.get('ESP_ROM_ELF_DIR'): env['ESP_ROM_ELF_DIR']=meoave.esp_rom_elf_dir(config)
    cmake=meoave.tool_path(config,'cmake','cmake')
    configure=[cmake,'-S',str(project),'-B',str(build),'-G','Ninja',
        '-DIDF_TARGET=esp32c5',f'-DPYTHON={meoave.python_command(config)}','-DPYTHON_DEPS_CHECKED=1',
        '-DESP_PLATFORM=1','-DCCACHE_ENABLE=False',f'-DSDKCONFIG={project/"sdkconfig"}',
        '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults']
    signature=hashlib.sha256((project/'sdkconfig.defaults').read_bytes()).hexdigest()
    sigfile=build/'.defaults_sha'
    if not (build/'build.ninja').is_file() or not sigfile.is_file() or sigfile.read_text()!=signature:
        # Regenerate only the diagnostic sdkconfig, never shared production settings.
        if (project/'sdkconfig').is_file(): (project/'sdkconfig').unlink()
        meoave.run(configure,project,env,'configure minimal C5')
        sigfile.write_text(signature)
    meoave.run([meoave.tool_path(config,'ninja','ninja'),'-C',str(build),'-j',str(meoave.cfg(config,'build','jobs',8))],
        project,env,'build minimal C5')
    app=(build/'meoave_c5_diag.bin').read_bytes()
    image=inspection.inspect_image(app,0,len(app))
    facts=json.loads(args.facts_report.read_text(encoding='utf-8'))
    if not facts.get('ok') or not facts.get('facts'): raise ValueError('Verified chip facts required')
    revision=facts['facts']['revision_full']
    if not image['present'] or revision<image['min_rev_full'] or (image['max_rev_full'] not in (0,65535) and revision>image['max_rev_full']):
        raise ValueError('Diagnostic image incompatible with actual chip revision')
    if len(app)>0x1e0000 or image.get('project')!='meoave_c5_diag': raise ValueError('Invalid diagnostic app')
    output=args.work_dir
    diagnostic=stage_payload(output/'diagnostic',app,'diagnostic',source['sha256'])
    # Exact complete ota_0 restore, including all old trailing bytes. No NVS/factory writes.
    restore=stage_payload(output/'restore',data[0x20000:0x200000],'restore',source['sha256'])
    result={'ok':True,'scope':'offline prepared; not flashed or runtime tested',
        'ram_diagnostic':'SKIPPED: current SDIO loader must reboot from overlapping flasher stub; safe RAM handoff not implemented',
        'backup_report':str(args.backup_report.resolve()),'backup_sha256':source['sha256'],
        'chip_revision':revision,'diagnostic_image':image,
        'diagnostic_payload_sha256':hashlib.sha256(diagnostic.read_bytes()).hexdigest(),
        'diagnostic_payload':str(diagnostic.resolve()),'restore_payload':str(restore.resolve()),
        'restore_app_sha256':hashlib.sha256(data[0x20000:0x200000]).hexdigest(),
        'original_elf_sha':old['applications']['ota_0'].get('elf_sha'),
        'preserved':['bootloader','partition-table','otadata','nvs','phy','fctry']}
    (output/'prepared.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
    meoave.build(config,'programmer',c5_payload=diagnostic)
    return result


def run_test(config,args):
    observe=getattr(args,'observe',False)
    if not observe and not args.straps_released: raise ValueError('--run requires --straps-released after user confirmation')
    prepared=json.loads((args.work_dir/'prepared.json').read_text(encoding='utf-8'))
    if not prepared.get('ok'): raise ValueError('Preparation was not verified')
    expected=prepared['diagnostic_image']['elf_sha']
    build=meoave.project_dir(config)/meoave.cfg(config,'build','programmer_dir','build_meoave_programmer')
    p4_sha=program.usb.image_identity(build)['elf_sha']
    report={'ok':False,'lines':[],'scope':'passive SDIO shared-register capture; not application acceptance' if observe else 'minimal C5 app only; not Hosted/Wi-Fi',
        'c5_reset':'not_requested','started':datetime.now().astimezone().isoformat()}
    try:
        if getattr(args,'install',False):
            if not observe: raise ValueError('--install allowed only with --observe; --run must use existing session')
            program.usb.flash_once(build,report=report.setdefault('p4_install',{}))
        before=program.usb.check_application(p4_sha,10);report['before']=before
        with program.usb.open_native(before['port'],program.usb.APP_PIDS) as stream:
            initial=exchange(stream,expected,report,observe=observe)
            if initial['state']!='IDLE': raise RuntimeError('Requires fresh diagnostic IDLE; no repeat')
            if not observe:
                program.require_radar_inactive(stream,report)
                report['c5_reset']='once_requested'
            try:
                report['result']=exchange(stream,expected,report,run=True,observe=observe)
            finally:
                status=report.get('last_status',{})
                if status.get('reset_completed'): report['c5_reset']='once_completed'
                if status.get('operation') and status.get('state') in ('FAILED','PASS','OBSERVED'):
                    if not observe:
                        try:
                            report['reset_trace']=read_reset_trace(stream,status['operation'])
                            if status.get('reset_completed') and not report['reset_trace']['pad_sequence_verified']:
                                raise RuntimeError('Reset completion without verified P4 pad sequence')
                        except Exception as reset_trace:
                            report['reset_trace_error']=str(reset_trace)
                            # A FAIL remains a FAIL; missing evidence must also block PASS.
                            if status.get('state')=='PASS': raise
                    try:
                        records=read_traces(stream,status['operation'],report,20 if observe and status['state']=='OBSERVED' else None)
                        report['trace_summary']=summarize_traces(records)
                    except Exception as trace:
                        report['trace_error']=str(trace)
                        if observe and status.get('state')=='OBSERVED': raise
        after=program.usb.check_application(p4_sha,10);report['after']=after
        if before['boot_id']!=after['boot_id']: raise RuntimeError('P4 reset during diagnosis')
        report['fault']=program.usb.read_fault_diagnostic(after['port'])
        if report['fault'].get('valid')!='0': raise RuntimeError('P4 fault status not clear')
        program.usb.check_fls2_runtime(after['port'],10,report.setdefault('runtime',{}))
        if observe:
            if report['result']['reset_completed'] or report['result']['state']!='OBSERVED':
                raise RuntimeError('Passive capture altered/reset C5')
            print('CAPTURED: no C5 reset/Flash write/echo; application NOT_VERIFIED',report['trace_summary'])
        else:
            print('PASS: C5 app identity + progressing heartbeat + 16 fresh CMD53 echo packets; NOT Hosted/Wi-Fi')
        report['ok']=True
        return 0
    except Exception as exc:
        report['ok']=False
        report['error']=f'{type(exc).__name__}: {exc}'
        print('FAIL:',report['error'],flush=True)
        try:
            report['after']=program.usb.check_application(p4_sha,10)
            report['fault']=program.usb.read_fault_diagnostic(report['after']['port'])
            program.usb.check_fls2_runtime(report['after']['port'],10,report.setdefault('runtime',{}))
        except Exception as health: report['health_error']=str(health)
        return 1
    finally:
        report['finished']=datetime.now().astimezone().isoformat()
        program.usb.write_report(args.report,report)


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    action=p.add_mutually_exclusive_group(required=True)
    action.add_argument('--prepare',action='store_true')
    action.add_argument('--flash',action='store_true')
    action.add_argument('--run',action='store_true')
    action.add_argument('--observe',action='store_true',help='one passive shared-register capture; SDIO init only, no C5 reset/Flash write/FIFO access')
    action.add_argument('--build-restore',action='store_true')
    action.add_argument('--restore',action='store_true',help='build/install app-only original restore, then write/verify; hold download straps')
    p.add_argument('--straps-released',action='store_true')
    p.add_argument('--install',action='store_true',help='install P4 only, with --observe; never writes C5')
    p.add_argument('--reset-download',action='store_true',help='explicit single C5 reset before flashing; all download straps must be held')
    p.add_argument('--backup-report',type=Path,default=RESULTS/'c5_read_backup_20261110.json')
    p.add_argument('--facts-report',type=Path,default=RESULTS/'c5_revision_facts_20261110.json')
    p.add_argument('--work-dir',type=Path,default=RESULTS/'c5_minimal_staging')
    p.add_argument('--report',type=Path,default=RESULTS/'c5_minimal_run.json')
    args=p.parse_args(argv);config=meoave.load_config()
    if args.prepare: prepare(config,args);return 0
    if args.run or args.observe: return run_test(config,args)
    prepared=json.loads((args.work_dir/'prepared.json').read_text(encoding='utf-8'))
    data,source,info=backup_data(Path(prepared['backup_report']))
    if source['sha256']!=prepared['backup_sha256']: raise ValueError('Backup changed')
    if args.build_restore or args.restore:
        payload=Path(prepared['restore_payload'])
        if (payload.parent/'application.bin').read_bytes()!=data[0x20000:0x200000]:
            raise ValueError('Restore payload does not match backup')
        meoave.build(config,'programmer',c5_payload=payload)
        if args.build_restore: return 0
    else:
        payload=Path(prepared['diagnostic_payload'])
        if hashlib.sha256(payload.read_bytes()).hexdigest()!=prepared['diagnostic_payload_sha256']:
            raise ValueError('Diagnostic payload changed since preparation')
    meoave.programmer_payload_layout(payload)
    build=meoave.project_dir(config)/meoave.cfg(config,'build','programmer_dir','build_meoave_programmer')
    command=[meoave.python_command(config),str(ROOT/'tools/c5_program.py'),
        '--build-dir',str(build),'--payload',str(payload),'--install','--flash','--report',str(args.report)]
    if args.reset_download: command.append('--reset-download')
    return subprocess.run(command,cwd=ROOT,env=meoave.environment(config)).returncode


if __name__=='__main__': raise SystemExit(main())
