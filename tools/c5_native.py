#!/usr/bin/env python3
"""Custom-board native USB -> P4 -> SDIO C5 maintenance (no COM3/UART).

check defaults to a reset-free ATTACH. --reset-c5 explicitly requests one EN
pulse with radar/audio inactive. An unresponsive C5 is NOT automatically reset,
reflashed or classified as damaged. No bootloader/strap writes in this tool.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import secrets
import sys
import time

import c5_maintenance as m
import c5_program
import meoave

sys.path.insert(0,str(meoave.project_dir(meoave.load_config())))
import flash_usb as usb

DEFAULT_BUILD=meoave.ROOT/'firmware/p4_gesture/build_meoave_maintenance'


def validate_config(config):
    required=('APP_C5_NATIVE_MAINTENANCE','APP_MEOAVE_UI','APP_FLS2_OFFICIAL_BASELINE',
              'APP_USB_HID_ENABLE','ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER')
    if any(not config.get(k) for k in required):
        raise RuntimeError('Native product maintenance/UI/USB/peer-data configuration missing')
    if config.get('APP_C5_SDIO_LOADER') or config.get('APP_AUDIO_BEEP'):
        raise RuntimeError('A competing SDIO loader or audio owner is enabled')


class NativeLink(m.Link):
    def __init__(self,stream,expected_elf,report,port='COM10'):
        super().__init__(stream,expected_elf,report)
        self.port=port

    def transaction(self,command,timeout=12):
        if not command or '\n' in command or '\r' in command or not command.isascii():
            raise ValueError('One ASCII command per native frame required')
        request=secrets.token_hex(6)
        wire=f'C5M {request} {command}'
        if len(wire)>=1400 or len(command)>=1360: raise ValueError('Command exceeds native receive frame')
        self.send(wire)  # exactly once; only queries may be explicitly repeated
        deadline=time.monotonic()+timeout
        begun=False
        lines=[]
        while True:
            line=self.line(deadline)
            if line.startswith('MEO:NATIVE_REJECT '):
                raise RuntimeError('Native command rejected; no retry: '+line)
            if line.startswith('MEO:NATIVE_BEGIN '):
                if begun or m.fields(line,'MEO:NATIVE_BEGIN').get('id')!=request:
                    raise RuntimeError('Native begin identity/order mismatch')
                begun=True
            elif line.startswith('MEO:NATIVE_END '):
                end=m.fields(line,'MEO:NATIVE_END')
                if not begun or end.get('id')!=request or end.get('tx_ok')!='1' or int(end['init_error'],0):
                    raise RuntimeError('Native transaction incomplete/identity mismatch; outcome not retried')
                return lines
            elif begun:
                lines.append(line)

    def identify(self,timeout=5):
        lines=self.transaction('INFO',timeout)
        ids=[m.fields(s,'MEO:BRINGUP') for s in lines if s.startswith('MEO:BRINGUP ')]
        states=[m.fields(s,'MEO:MAINT_STATUS') for s in lines if s.startswith('MEO:MAINT_STATUS ')]
        if len(ids)!=1 or len(states)!=1 or ids[0].get('project')!='p4_gesture' or ids[0].get('profile')!='maintenance' or ids[0].get('elf_sha')!=self.expected_elf:
            raise RuntimeError('Native P4 image/profile identity mismatch')
        self.report.setdefault('p4_identities',[]).append(ids[0])
        return states[0]

    def command(self,command,timeout=12,token=None,offset=None):
        op=command.split()[0]
        lines=self.transaction(command,timeout)
        replies=[m.fields(s,'MEO:MAINT_REPLY') for s in lines if s.startswith('MEO:MAINT_REPLY ')]
        if len(replies)!=1 or replies[0].get('op')!=op:
            raise RuntimeError('Missing/duplicate/mismatched native maintenance reply')
        reply=replies[0]
        if token is not None and reply.get('token')!=token: raise RuntimeError('OTA token mismatch')
        if int(reply['error'],0): raise m.MaintenanceCommandError(op,reply,lines)
        if offset is not None and int(reply['offset'])!=offset: raise RuntimeError('OTA offset mismatch; no retry')
        return lines,reply

    def reset_p4(self):
        # The product SYS path preserves GPIO29, USB recovery and panel warm init.
        # Never send the headless P4RESET/esp_restart command to this board.
        self.stream.close()
        self.report.setdefault('p4_restarts',[]).append({})
        usb.restart_once(self.expected_elf,30,self.report['p4_restarts'][-1])
        port=usb.app_port()
        if not port: raise RuntimeError('Native USB absent after explicit P4 restart')
        self.port=port.device
        self.stream=usb.open_native(self.port,usb.APP_PIDS)
        self.pending=b''
        if self.identify(10)['state']!='IDLE': raise RuntimeError('Native P4 did not restart IDLE')


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=('check','update'))
    p.add_argument('--port',help='native application USB only; auto-detect if omitted')
    p.add_argument('--p4-build',type=Path,default=DEFAULT_BUILD)
    p.add_argument('--image',type=Path,default=m.DEFAULT_IMAGE)
    p.add_argument('--reset-c5',action='store_true',help='explicit one C5 EN pulse before attachment; radar must be inactive')
    p.add_argument('--report',type=Path,required=True)
    a=p.parse_args(argv)
    if a.report.exists(): p.error('Report exists; use a fresh evidence filename')
    r=dict(ok=False,started=datetime.now().astimezone().isoformat(),
           scope='custom-board native USB/P4/SDIO; not development UART or screen visual acceptance',
           c5_reset='requested_not_yet_sent' if a.reset_c5 else 'never_requested',
           c5_straps_changed=False,c5_firmware_write_requested=a.mode=='update')
    link=None
    try:
        config=json.loads((a.p4_build/'config/sdkconfig.json').read_text(encoding='utf-8'))
        validate_config(config)
        info=usb.image_identity(a.p4_build)
        before=r['before']=usb.check_application(info['elf_sha'],10)
        if a.port and a.port.casefold()!=before['port'].casefold(): raise RuntimeError('Selected port is not the verified native P4')
        link=NativeLink(usb.open_native(before['port'],usb.APP_PIDS),info['elf_sha'],r,before['port'])
        state=link.identify()
        if state['state']=='IDLE':
            if a.reset_c5:
                c5_program.require_radar_inactive(link.stream,r)
                r['c5_reset']='connect_once_requested'
            link.command('CONNECT' if a.reset_c5 else 'ATTACH',timeout=40)
        elif state['state']!='READY': raise RuntimeError('Non-ready native host; no hidden reset/retry')
        baseline=r['initial_c5']=link.inspect()
        if a.mode=='update':
            image=a.image.read_bytes();m.validate_candidate_mode(image,'update')
            meta=m.image_info(image)
            m.update(link,image,meta,'update')
        else:
            for _ in range(2):
                time.sleep(.5)
                current=link.inspect()
                if any(current[k]!=baseline[k] for k in ('boot','elf_sha','slot','ota_state')):
                    raise RuntimeError('C5 identity/state changed during read-only inspection')
        r['final_c5']=link.inspect()
        r['ok']=True
    except Exception as exc:
        r['error']=f'{type(exc).__name__}: {exc}'
        print(r['error'],flush=True)
    finally:
        if link: link.stream.close()
        if 'before' in r:
            try:
                r['after']=usb.check_application(r['before']['elf_sha'],8)
                r['fault']=usb.read_fault_diagnostic(r['after']['port'])
                if r['fault'].get('valid')!='0': raise RuntimeError('P4 fault status not clear')
                if a.mode=='check' and r['before']['boot_id']!=r['after']['boot_id']:
                    raise RuntimeError('Unexpected P4 reboot during check')
            except Exception as exc:
                r['ok']=False;r['health_error']=str(exc)
        r['finished']=datetime.now().astimezone().isoformat()
        a.report.parent.mkdir(parents=True,exist_ok=True)
        temp=a.report.with_suffix('.json.tmp')
        temp.write_text(json.dumps(r,ensure_ascii=False,indent=2),encoding='utf-8')
        temp.replace(a.report)
    print('PASS' if r['ok'] else 'NOT VERIFIED',flush=True)
    return 0 if r['ok'] else 1


if __name__=='__main__': raise SystemExit(main())
