#!/usr/bin/env python3
"""Read C5 Flash through P4 native USB + SDIO; never erase/write/reset C5.

Requires manually entered SDIO download mode. Official flasher stub runs in
C5 RAM. --install updates P4 only. A partial backup is never a verified backup.
"""
from __future__ import annotations
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import sys
import time

import meoave
import c5_program as program

usb = program.usb
READ = re.compile(r'MEO:C5_READ token=(\w{1,16}) operation=(\w{1,16}) '
    r'state=(IDLE|QUEUED|READING|DONE|FAILED) offset=0x([0-9a-f]{8}) '
    r'size=(\d+) error=(\d+) md5=(none|[0-9a-f]{32})')
DATA = re.compile(r'MEO:C5_DATA token=(\w{1,16}) pos=(\d+) size=(\d+) data=([0-9a-f]*)')


class Reader:
    def __init__(self, stream, clock=time.monotonic):
        self.stream, self.clock, self.pending = stream, clock, b''

    def send(self, command):
        self.stream.write((command+'\n').encode('ascii'))
        self.stream.flush()

    def line(self):
        if b'\n' not in self.pending:
            self.pending += self.stream.read(4096)
        if b'\n' not in self.pending:
            if len(self.pending) > 8192:
                raise ValueError('Unterminated output')
            return None
        raw, self.pending = self.pending.split(b'\n', 1)
        return raw.decode('ascii', errors='replace').rstrip('\r')

    def block(self, offset, size):
        if not 0 <= offset < 0x400000 or not 1 <= size <= 4096 or size > 0x400000-offset:
            raise ValueError('Read outside supported flash/cache bounds')
        token = secrets.token_hex(6)
        self.send(f'C5 READ {token} {offset:x} {size}')  # exactly once
        end, poll, digest = self.clock()+8, 0, None
        while self.clock() < end:
            if self.clock() >= poll:
                self.send(f'C5 READSTATUS {token}')
                poll = self.clock()+.1
            line = self.line()
            if line is None:
                continue
            if line == f'MEO:C5_READ_ACCEPT token={token} accepted=0':
                raise RuntimeError('Read rejected; not repeating READ')
            match = READ.fullmatch(line)
            if not match or match[1] != token or match[2] != token:
                continue
            if (int(match[4],16), int(match[5])) != (offset,size):
                raise ValueError('Read result address/length mismatch')
            if match[3] == 'FAILED' or int(match[6]):
                raise RuntimeError(f'Flash read failed: {line}')
            if match[3] == 'DONE':
                if match[7] == 'none':
                    raise ValueError('Completed read missing digest')
                digest = match[7]
                break
        if digest is None:
            raise TimeoutError('Read completion missing; no automatic restart')
        out = bytearray()
        for pos in range(0,size,128):
            got = None
            # DATA only retrieves the completed cache. Re-querying is passive.
            for attempt in range(3):
                self.send(f'C5 DATA {token} {pos}')
                end = self.clock()+.3
                while self.clock() < end:
                    line = self.line()
                    if line is None:
                        continue
                    match = DATA.fullmatch(line)
                    if not match or match[1] != token or int(match[2]) != pos:
                        continue
                    got = bytes.fromhex(match[4])
                    expected = min(128,size-pos)
                    if len(got) != expected or int(match[3]) != expected:
                        raise ValueError('Cached data length mismatch')
                    break
                if got is not None:
                    break
            if got is None:
                raise TimeoutError('Cached data unavailable; no new READ/reset')
            out.extend(got)
        if hashlib.md5(out).hexdigest() != digest:
            raise ValueError('USB cache MD5 mismatch')
        return bytes(out)


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--install',action='store_true',help='install P4 programmer only')
    p.add_argument('--limit',type=lambda x:int(x,0),default=0x400000)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True)
    args=p.parse_args(argv)
    if not 1 <= args.limit <= 0x400000:
        p.error('limit must be 1..0x400000')
    config=meoave.load_config()
    build=meoave.project_dir(config)/meoave.cfg(config,'build','programmer_dir','build_meoave_programmer')
    payload=meoave.c5_dir(config)/meoave.cfg(config,'build','c5_dir','build_c5')/'c5_hosted_slave_merged.bin'
    report={'ok':False,'completed':False,'c5_flash_written':False,'c5_reset':'never',
            'gpio6_driven':False,'scope':'ROM download plus RAM flasher stub; not Hosted runtime',
            'started':datetime.now().astimezone().isoformat(),'lines':[],'bytes_saved':0}
    output=args.output.resolve()
    partial=output.with_name(output.name+'.partial')
    try:
        if output.exists() or partial.exists():
            raise FileExistsError('Refusing to overwrite existing backup/partial file')
        sha=usb.image_identity(build)['elf_sha']
        report['p4_elf_sha']=sha
        if args.install:
            report['install']={}
            usb.flash_once(build,report=report['install'])
        report['before']=usb.check_application(sha,10)
        payload_sha=hashlib.sha256(payload.read_bytes()).hexdigest()
        with usb.open_native(report['before']['port'],usb.APP_PIDS) as stream:
            initial=program.exchange(stream,None,payload_sha,report,5)
            if initial['state']=='IDLE':
                report['connected']=program.exchange(stream,'CONNECT',payload_sha,report,30)
            elif initial['state']=='CONNECTED':
                report['connected']=initial
            else:
                raise RuntimeError('Programmer not idle/connected; no reset/retry')
            if report['connected']['flash_size'] != 0x400000:
                raise ValueError('Expected exact 4 MB C5 flash before backup')
            stream.timeout=.005
            reader=Reader(stream)
            # Repeat a small read before admitting a complete backup operation.
            first=reader.block(0x2000,256)
            if first != reader.block(0x2000,256):
                raise ValueError('Repeated small read differs')
            report['small_read']={'offset':0x2000,'size':256,'repeat_equal':True,
                                  'sha256':hashlib.sha256(first).hexdigest(),'head':first[:32].hex()}
            print('PASS: repeated small Flash read + target/USB digests',flush=True)
            output.parent.mkdir(parents=True,exist_ok=True)
            digest=hashlib.sha256()
            deadline=time.monotonic()+900
            with partial.open('xb') as f:
                for offset in range(0,args.limit,4096):
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Backup exceeded 15-minute bound')
                    data=reader.block(offset,min(4096,args.limit-offset))
                    f.write(data); digest.update(data)
                    report['bytes_saved']=offset+len(data)
                    if offset % 0x40000 == 0:
                        print(f'READ {report["bytes_saved"]}/{args.limit}',flush=True)
                        usb.write_report(args.report,report)
            # Independently repeat the startup region, not just the first header.
            with partial.open('rb') as f:
                for offset in range(0,min(args.limit,0x20000),4096):
                    size=min(4096,min(args.limit,0x20000)-offset)
                    if reader.block(offset,size) != f.read(size):
                        raise ValueError(f'Startup readback differs at {offset:#x}')
            report['startup_repeat_verified']=min(args.limit,0x20000)
            report['sha256']=digest.hexdigest()
        report['after']=usb.check_application(sha,10)
        if report['after']['boot_id'] != report['before']['boot_id']:
            raise RuntimeError('P4 rebooted during backup')
        report['fault']=usb.read_fault_diagnostic(report['after']['port'])
        if report['fault'].get('valid')!='0':
            raise RuntimeError('P4 fault not clear')
        report['runtime']={}
        usb.check_fls2_runtime(report['after']['port'],10,report['runtime'])
        saved=partial.read_bytes()
        if len(saved)!=args.limit or hashlib.sha256(saved).hexdigest()!=report['sha256']:
            raise ValueError('Saved backup length/digest mismatch')
        partial.rename(output)
        report.update(ok=True,completed=True,output=str(output),full_flash=args.limit==0x400000)
        print(f'PASS: saved {len(saved)} bytes; C5 Flash not written',flush=True)
        return 0
    except Exception as exc:
        report['error']=f'{type(exc).__name__}: {exc}'
        print('FAIL: '+report['error'],flush=True)
        return 1
    finally:
        report['finished']=datetime.now().astimezone().isoformat()
        usb.write_report(args.report,report)
        print('REPORT '+str(args.report.resolve()),flush=True)


if __name__=='__main__':
    raise SystemExit(main())
