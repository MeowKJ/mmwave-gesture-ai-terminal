#!/usr/bin/env python3
"""Provision a manually bootstrapped C5 through P4 USB + SDIO.

Install the programmer profile only with --install. Erase/write C5 image
regions only with --flash. --reset-download explicitly requests one C5 reset
via shared GPIO54, with radar inactive; otherwise no reset. Never touches GPIO6.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware/p4_gesture"))
import flash_usb as usb

STATUS = re.compile(
    r'MEO:C5_LOADER token=(?P<token>[a-zA-Z0-9]{1,16}) operation=(?P<operation>[a-zA-Z0-9]{1,16}) '
    r'state=(?P<state>IDLE|QUEUED|CONNECTING|CONNECTED|WRITING|DONE|FAILED) '
    r'stage=(?P<stage>[A-Z0-9_]+) error=(?P<error>\d+) chip=(?P<chip>\d+) '
    r'flash_size=(?P<flash_size>\d+) written=(?P<written>\d+) total=(?P<total>\d+) '
    r'verified=(?P<verified>[01]) payload=(?P<payload>[0-9a-f]{64}) reset=(?P<reset>never|once)')
ACK = re.compile(r'MEO:C5_LOADER_ACCEPT token=([a-zA-Z0-9]{1,16}) accepted=([01])')
FACTS = re.compile(r'MEO:C5_FACTS token=([a-zA-Z0-9]{1,16}) valid=([01]) '
    r'rev_raw=0x([0-9a-f]{8}) strap=0x([0-9a-f]{8}) lp_sys=0x([0-9a-f]{8}) '
    r'reset_cause=0x([0-9a-f]{8}) reset_completed=([01])'
    r'(?: gpio_in=0x([0-9a-f]{8}) gpio_enable=0x([0-9a-f]{8}) '
    r'mux26=0x([0-9a-f]{8}) mux27=0x([0-9a-f]{8}) mux28=0x([0-9a-f]{8}))?')


def decode_boot_pins(gpio_in, gpio_enable, muxes, strap):
    """CONNECT-time snapshot, not a live read, EN-edge voltage or boot log."""
    pins={}
    for pin,mux in zip((26,27,28),muxes):
        enabled=bool(mux & (1<<9))
        pins[str(pin)]={'mux_raw':mux,'input_enabled':enabled,
            'level':((gpio_in>>pin)&1) if enabled else None,
            'gpio_output_enabled':bool(gpio_enable & (1<<pin)),
            'pull_up':bool(mux & (1<<8)),'pull_down':bool(mux & (1<<7))}
    return {'gpio_in':gpio_in,'gpio_enable':gpio_enable,'pins':pins,
        'latched_gpio27':(strap>>3)&1,'latched_gpio28':(strap>>4)&1,
        'scope':'CONNECT-time snapshot; not live GPIO, EN-edge voltage or prior application boot log'}


def require_radar_inactive(stream, report):
    stream.write(b'UI\n'); stream.flush()
    pending=b''; end=time.monotonic()+4
    while time.monotonic()<end:
        pending+=stream.read(4096)
        while b'\n' in pending:
            raw,pending=pending.split(b'\n',1)
            line=raw.decode('ascii',errors='replace').rstrip('\r')
            if line.startswith('MEO:UI '):
                if not re.search(r'(?:^| )radar=0(?: |$)',line):
                    raise RuntimeError('Radar not inactive: refusing GPIO54 reset')
                report['reset_guard_ui']=line
                return
        pending=pending[-8192:]
    raise TimeoutError('Radar inactive not confirmed: no GPIO54 reset requested')


def read_facts(stream, report):
    """Fetch the CONNECT cache. A fresh USB token does not refresh C5 registers.

    Only the caller that observes IDLE then completes a new CONNECT can prove
    this cache was captured during its measurement window.
    """
    token=secrets.token_hex(6); pending=b''; end=time.monotonic()+4; poll=0
    while time.monotonic()<end:
        if time.monotonic()>=poll:
            stream.write(f'C5 FACTS {token}\n'.encode());stream.flush()
            poll=time.monotonic()+.5
        pending+=stream.read(4096)
        while b'\n' in pending:
            raw,pending=pending.split(b'\n',1)
            line=raw.decode('ascii',errors='replace').rstrip('\r')
            m=FACTS.fullmatch(line)
            if not m or m[1]!=token: continue
            report['facts_record']=line
            if m[2]!='1': raise RuntimeError('Target facts not valid')
            raw_revision=int(m[3],16)
            facts={'revision_raw':raw_revision,'revision_major':(raw_revision>>4)&3,
                'revision_minor':raw_revision&15,'revision_full':((raw_revision>>4)&3)*100+(raw_revision&15),
                'strap':int(m[4],16),'lp_sys':int(m[5],16),'reset_cause':int(m[6],16),
                'reset_completed':m[7]=='1','sample_source':'CONNECT_CACHE',
                'live_gpio_read':False,
                'scope':'CONNECT-time cache; sample age unknown without new-connection provenance; not prior application boot log'}
            if m[8] is not None:
                facts['boot_pins']=decode_boot_pins(int(m[8],16),int(m[9],16),
                    [int(m[i],16) for i in (10,11,12)],int(m[4],16))
            return facts
        pending=pending[-8192:]
    raise TimeoutError('No valid cached target facts response')


def require_new_facts_connection(initial_state):
    """Never present a reused CONNECT cache as a new hardware measurement."""
    if initial_state != 'IDLE':
        raise RuntimeError('--facts requires fresh IDLE then a new CONNECT; existing cache is not a live GPIO read; no automatic reset/reconnect')


def parse_status(line, payload_sha):
    m = STATUS.fullmatch(line)
    if not m:
        raise ValueError('Malformed C5 programmer record')
    s = m.groupdict()
    for key in ('error', 'chip', 'flash_size', 'written', 'total', 'verified'):
        s[key] = int(s[key])
    if s['payload'] != payload_sha:
        raise RuntimeError('Embedded C5 payload differs from the requested local image')
    if s['written'] > s['total']:
        raise ValueError('Impossible write progress')
    if s['state'] in ('CONNECTED', 'DONE'):
        if s['error'] or s['chip'] != 6 or s['flash_size'] < 0x400000:
            raise ValueError('C5 chip/flash connection not verified')
    if s['state'] == 'DONE' and (not s['verified'] or not s['total'] or s['written'] != s['total']):
        raise ValueError('Completion without complete writes and verification')
    return s


def exchange(stream, command, payload_sha, report, timeout, clock=time.monotonic):
    token = secrets.token_hex(6)
    pending = b''
    deadline, next_poll = clock() + timeout, 0
    # CONNECT/FLASH are sent once. Lost ACKs are recovered by observing the
    # operation nonce; they must never cause another erase or write request.
    if command:
        stream.write(f'C5 {command} {token}\n'.encode('ascii'))
        stream.flush()
    last_progress = None
    while clock() < deadline:
        if clock() >= next_poll:
            stream.write(f'C5 STATUS {token}\n'.encode('ascii'))
            stream.flush()
            next_poll = clock() + .5
        pending += stream.read(4096)
        while b'\n' in pending:
            raw, pending = pending.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace').rstrip('\r')
            if len(report['lines']) < 4000:
                report['lines'].append(line)
            if line.startswith('MEO:C5_LOADER_ACCEPT '):
                ack = ACK.fullmatch(line)
                if not ack:
                    raise ValueError('Malformed programmer ACK')
                if ack[1] == token and ack[2] == '0':
                    raise RuntimeError(f'{command} rejected; no automatic retry')
            if line.startswith('MEO:C5_LOADER '):
                s = parse_status(line, payload_sha)
                if s['token'] != token:
                    continue
                if command and s['operation'] != token:
                    continue
                report['last_status'] = s
                progress = (s['state'], s['stage'], int(100*s['written']/max(s['total'], 1))//10)
                if progress != last_progress:
                    print(f"C5 {s['state']} {s['stage']} {s['written']}/{s['total']} error={s['error']}", flush=True)
                    last_progress = progress
                if s['state'] == 'FAILED':
                    raise RuntimeError(f"C5 failed at {s['stage']}: loader_error={s['error']}; no reset/retry")
                if not command or (command in ('CONNECT','BOOTCONNECT') and s['state'] == 'CONNECTED') or (command == 'FLASH' and s['state'] == 'DONE'):
                    return s
        if len(pending) > 4096:
            raise ValueError('Unbounded programmer output')
    raise TimeoutError(f'C5 {command or "STATUS"} did not complete; no reset/retry')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--flash', action='store_true')
    parser.add_argument('--reset-download', action='store_true',
                        help='one explicit C5 GPIO54 reset before CONNECT; hold download straps, radar must be inactive')
    parser.add_argument('--facts', action='store_true', help='require IDLE then new CONNECT-time C5 register capture; rejects reused cache, not a live refresh command')
    parser.add_argument('--report', type=Path, default=ROOT/'firmware/p4_gesture/test-results/c5_program.json')
    args = parser.parse_args()
    report = dict(ok=False, started=datetime.now().astimezone().isoformat(), lines=[],
                  c5_reset='never', c5_runtime='NOT_VERIFIED', wifi='NOT_VERIFIED')
    try:
        payload_sha = hashlib.sha256(args.payload.read_bytes()).hexdigest()
        report['payload_sha256'] = payload_sha
        manifest = json.loads((args.payload.parent/'flasher_args.json').read_text(encoding='utf-8'))
        if manifest.get('extra_esptool_args', {}).get('chip') != 'esp32c5':
            raise ValueError('Not an ESP32-C5 payload')
        import meoave
        meoave.programmer_payload_layout(args.payload)  # reject stale merged content before USB
        expected_total = sum((args.payload.parent/f).stat().st_size for f in manifest['flash_files'].values())
        expected_sha = usb.image_identity(args.build_dir)['elf_sha']
        if args.install:
            usb.flash_once(args.build_dir, report=report.setdefault('p4_install', {}))
        before = usb.check_application(expected_sha, 15)
        report['before'] = before
        with usb.open_native(before['port'], usb.APP_PIDS) as stream:
            initial = exchange(stream, None, payload_sha, report, 5)
            if args.facts:
                require_new_facts_connection(initial['state'])
            reused = False
            if initial['state'] == 'IDLE':
                if args.reset_download:
                    require_radar_inactive(stream,report)
                    report['c5_reset']='once_requested'
                connected = exchange(stream, 'BOOTCONNECT' if args.reset_download else 'CONNECT', payload_sha, report, 30)
            elif initial['state'] == 'CONNECTED':
                if args.reset_download:
                    raise RuntimeError('Reset only permitted from fresh IDLE; no automatic recovery reset')
                connected = initial
            elif initial['state'] == 'DONE' and args.flash:
                # Same power cycle already wrote and MD5-verified this payload.
                # Reusing that result is read-only; FLASH from DONE is rejected.
                connected = initial
                reused = True
                report['flash_reused'] = True
            else:
                raise RuntimeError('Programmer is not idle/connected; no automatic retry')
            report['connected'] = connected
            if args.facts or args.reset_download:
                report['facts']=read_facts(stream,report)
                report['fresh_connection_capture']=initial['state']=='IDLE'
                report['facts']['new_connection_in_this_run']=report['fresh_connection_capture']
                if args.reset_download:
                    if connected['reset']!='once' or not report['facts']['reset_completed']:
                        raise RuntimeError('One-shot reset completion not verified')
                    report['c5_reset']='once_completed'
            if args.flash:
                result = initial if reused else exchange(stream, 'FLASH', payload_sha, report, 210)
                if result['total'] != expected_total:
                    raise RuntimeError('Verified length differs from the selected local flash images')
                report['flash'] = result
        after = usb.check_application(expected_sha, 10)
        report['after'] = after
        if before['boot_id'] != after['boot_id'] or after['uptime_us'] <= before['uptime_us']:
            raise RuntimeError('P4 reset/stall during C5 operation')
        report['fault'] = usb.read_fault_diagnostic(after['port'])
        if report['fault'].get('valid') != '0':
            raise RuntimeError('P4 fault diagnostic is not clear')
        report['ok'] = True
        if args.flash:
            note = ' (reused verified result from this power cycle)' if report.get('flash_reused') else ''
            print(f'PASS: C5 image regions written and MD5 verified{note}; C5 boot/runtime pending', flush=True)
        else:
            print('PASS: C5 identified and connected; flash not requested', flush=True)
        return 0
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print('FAIL: '+report['error'], flush=True)
        return 1
    finally:
        report['finished'] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == '__main__':
    raise SystemExit(main())
