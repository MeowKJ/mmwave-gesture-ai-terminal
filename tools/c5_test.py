#!/usr/bin/env python3
"""Bounded C5 Hosted speed/width matrix, through P4 native USB only.

Build/install once; one fresh P4 program restart and one C5 reset per case.
Never calls the independent PINS/PROBE diagnostics, writes C5 flash, toggles
GPIO6, uses UART/COM3, or retries a failed CONNECT. UI visual QA is separate.
"""
from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import re
import secrets
import sys
import time

import meoave

sys.path.insert(0, str(meoave.project_dir(meoave.load_config())))
import flash_usb as usb

MATRIX = ((1, 400), (1, 5000), (4, 400), (4, 5000), (4, 20000), (4, 40000))
SPEEDS = (400, 1000, 5000, 10000, 20000, 40000)
STATUS_PREFIX = 'MEO:WIFI_HOSTED '


def parse_case(value):
    if not re.fullmatch(r'[14]:\d+', value):
        raise argparse.ArgumentTypeError('case must be WIDTH:KHZ, e.g. 1:400')
    width, khz = map(int, value.split(':'))
    if khz not in SPEEDS:
        raise argparse.ArgumentTypeError(f'KHZ must be one of {SPEEDS}')
    return width, khz


def parse_status(line):
    if not line.startswith(STATUS_PREFIX):
        raise ValueError('Not a Hosted status')
    pairs = [item.split('=', 1) for item in line.split()[1:]]
    if any(len(item) != 2 for item in pairs):
        raise ValueError('Malformed Hosted status')
    s = dict(pairs)
    if len(s) != len(pairs):
        raise ValueError('Duplicate Hosted field')
    required = {'token', 'state', 'error', 'chip', 'fw', 'wifi_started', 'proto',
                'operation', 'diag', 'width', 'khz', 'gpio6', 'reset_gpio', 'poll_ms', 'init_ms'}
    if not required <= s.keys() or s['proto'] != '2':
        raise ValueError('Diagnostic protocol v2 firmware required')
    if not re.fullmatch(r'[A-Za-z0-9]{1,16}', s['token']):
        raise ValueError('Invalid status token')
    if not re.fullmatch(r'[A-Za-z0-9]{1,16}', s['operation']):
        raise ValueError('Invalid operation token')
    if s['state'] not in ('IDLE', 'QUEUED', 'CONNECTING', 'READY', 'FAILED'):
        raise ValueError('Invalid Hosted state')
    for key in ('width', 'khz', 'wifi_started', 'diag', 'poll_ms', 'init_ms'):
        if not s[key].isdigit():
            raise ValueError('Invalid numeric status')
        s[key] = int(s[key])
    if s['width'] not in (1, 4) or s['khz'] not in SPEEDS or s['diag'] not in (0, 1) or s['wifi_started'] not in (0, 1):
        raise ValueError('Invalid diagnostic settings')
    if s['poll_ms'] not in (0, 100) or not 4000 <= s['init_ms'] <= 20000 or s['init_ms'] % 200:
        raise ValueError('Invalid receive/wait settings')
    if s['gpio6'] != 'UNUSED' or s['reset_gpio'] != '54':
        raise ValueError('Unexpected shared-pin policy')
    if not re.fullmatch(r'0x[0-9a-f]+', s['error']):
        raise ValueError('Invalid error field')
    if s['state'] == 'READY' and (s['chip'] != 'esp32c5' or
            not re.fullmatch(r'\d+\.\d+\.\d+', s['fw']) or
            s['wifi_started'] != 1 or int(s['error'], 16) != 0):
        raise ValueError('READY without verified C5 identity/version/Wi-Fi start')
    return s


def exchange(stream, command, token, report, *, timeout, predicate, clock=time.monotonic):
    """Send a mutating command exactly once. Recover lost ACK via STATUS only."""
    if not re.fullmatch(r'[A-Za-z0-9]{1,16}', token):
        raise ValueError('Invalid request token')
    pending = b''
    deadline, next_poll = clock() + timeout, 0
    if command:
        stream.write((command + '\n').encode('ascii'))
        stream.flush()
    while clock() < deadline:
        if clock() >= next_poll:
            stream.write(f'WIFI HOSTED STATUS {token}\n'.encode('ascii'))
            stream.flush()
            next_poll = clock() + .5
        pending += stream.read(4096)
        while b'\n' in pending:
            raw, pending = pending.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace').rstrip('\r')
            if len(report['lines']) < 6000:
                report['lines'].append(line)
            if line.startswith('MEO:REJECTED '):
                raise RuntimeError(line)
            if line.startswith(('MEO:WIFI_HOSTED_CONFIG ', 'MEO:WIFI_HOSTED_ACCEPT ')):
                fields = dict(part.split('=', 1) for part in line.split()[1:] if '=' in part)
                if fields.get('token') == token and fields.get('accepted') == '0':
                    raise RuntimeError('Command rejected; not retransmitting: ' + line)
            if line.startswith(STATUS_PREFIX):
                # Ignore unrelated status records before parsing new protocol.
                if not line.startswith(STATUS_PREFIX + f'token={token} '):
                    continue
                status = parse_status(line)
                report['last_status'] = status
                if predicate(status):
                    return status
        if len(pending) > 8192:
            raise ValueError('Unterminated USB output exceeds limit')
    raise TimeoutError('No matching complete status; not repeating command/reset')


def summarize_evidence(lines):
    # Missing/failed samples must never be reported as all-zero slave data.
    snapshots, snapshot_errors, invalid_snapshots = [], 0, 0
    fields = ('error', 'ocr', 'ien', 'pending', 'token', 'raw', 'st', 'len', 'ena')
    for line in lines:
        if 'MEO_SDIO_SNAPSHOT ' not in line:
            continue
        pairs = re.findall(r'(\w+)=(0x[0-9a-fA-F]+|\d+)\b',
                           line.split('MEO_SDIO_SNAPSHOT ', 1)[1])
        sample = dict(pairs)
        if len(sample) != len(pairs) or not set(fields) <= sample.keys():
            invalid_snapshots += 1
            continue
        values = {key: int(sample[key], 16) for key in fields}
        if values['error']:
            snapshot_errors += 1
        else:
            snapshots.append(values)
    return {
        'card_initialized': any('MEO_SDIO_TEST: card initialized' in x for x in lines),
        # STATUS error=0x107 may mean Hosted INIT timeout, not bus timeout.
        'crc_errors': sum('0x109' in x and 'sdmmc' in x for x in lines),
        'sdio_command_timeouts': sum('0x107' in x and any(tag in x for tag in
                                     ('sdmmc', 'SD_HOST:', 'SD_TRANS:')) for x in lines),
        'hosted_init_timeout': any(re.search(r'no Hosted init event within \d+ ms', x) for x in lines),
        'reset_sequences': sum('MEO_SDIO_TEST: one slave reset;' in x for x in lines),
        'snapshot_valid_reads': len(snapshots),
        'snapshot_read_errors': snapshot_errors,
        'snapshot_invalid_records': invalid_snapshots,
        'snapshot_data_all_zero': bool(snapshots) and not snapshot_errors and
            not invalid_snapshots and all(not s[key] for s in snapshots
                for key in ('pending', 'token', 'raw', 'st', 'len')),
    }


def run_case(sha, width, khz, timeout, soak, poll_ms=0, init_ms=4000):
    r = {'width': width, 'khz': khz, 'lines': [], 'ok': False,
         'poll_ms': poll_ms, 'init_ms': init_ms,
         'visual': 'NOT_RETESTED', 'started': datetime.now().astimezone().isoformat()}
    try:
        r['restart'] = {}
        usb.restart_once(sha, 30, r['restart'])
        before = r['before'] = r['restart']['after']
        token = r['token'] = secrets.token_hex(6)
        with usb.open_native(before['port'], usb.APP_PIDS) as stream:
            initial = exchange(stream, None, token, r, timeout=4,
                               predicate=lambda s: True)
            if initial['state'] != 'IDLE' or initial['diag']:
                raise RuntimeError('Fresh IDLE/unconfigured baseline missing')
            exchange(stream, f'WIFI HOSTED CONFIG {token} {width} {khz} {poll_ms} {init_ms}', token, r,
                     timeout=4, predicate=lambda s: s['state'] == 'IDLE' and
                     s['diag'] == 1 and (s['width'], s['khz'], s['poll_ms'], s['init_ms']) ==
                     (width, khz, poll_ms, init_ms))
            terminal = r['terminal'] = exchange(stream, f'WIFI HOSTED CONNECT {token}', token, r,
                     timeout=timeout, predicate=lambda s: s['operation'] == token and
                     s['state'] in ('READY', 'FAILED'))
            if terminal['diag'] != 1 or (terminal['width'], terminal['khz'], terminal['poll_ms'], terminal['init_ms']) != (width, khz, poll_ms, init_ms):
                raise RuntimeError('Terminal result belongs to different diagnostic settings')
            if terminal['state'] == 'READY':
                # Passive stability check, not a throughput or network test.
                end = time.monotonic() + soak
                while time.monotonic() < end:
                    stable = exchange(stream, None, token, r, timeout=3,
                                      predicate=lambda s: s['operation'] == token)
                    if stable['state'] != 'READY':
                        raise RuntimeError('Hosted READY did not remain stable')
                    time.sleep(.2)
        r['after'] = usb.check_application(sha, 10)
        if r['after']['boot_id'] != before['boot_id']:
            raise RuntimeError('P4 unexpectedly rebooted during case')
        r['fault'] = usb.read_fault_diagnostic(r['after']['port'])
        if r['fault'].get('valid') != '0':
            raise RuntimeError('P4 fault diagnostic not clear')
        r['runtime'] = {}
        usb.check_fls2_runtime(r['after']['port'], 10, r['runtime'])
        if sum('MEO_SDIO_TEST: one slave reset;' in x for x in r['lines']) != 1:
            raise RuntimeError('Expected exactly one C5 reset sequence in this case')
        r['ok'] = terminal['state'] == 'READY'
        r['completed'] = True
    except Exception as exc:
        r['error'] = f'{type(exc).__name__}: {exc}'
        r['completed'] = False
    r['evidence'] = summarize_evidence(r['lines'])
    r['finished'] = datetime.now().astimezone().isoformat()
    return r


def postflight(sha, report):
    report['application'] = usb.check_application(sha, 10)
    with usb.open_native(report['application']['port'], usb.APP_PIDS) as stream:
        report['lines'] = []
        report['hosted'] = exchange(stream, None, secrets.token_hex(6), report,
                                   timeout=4, predicate=lambda s: True)
    if report['hosted']['state'] != 'IDLE' or report['hosted']['diag']:
        raise RuntimeError('Final boot not in default IDLE state')
    report['fault'] = usb.read_fault_diagnostic(report['application']['port'])
    if report['fault'].get('valid') != '0':
        raise RuntimeError('Final P4 fault check not clear')
    report['runtime'] = {}
    usb.check_fls2_runtime(report['application']['port'], 10, report['runtime'])
    report['ok'] = True


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', action='store_true', help='incrementally build Hosted profile once')
    p.add_argument('--install', action='store_true', help='flash/verify P4 profile once, never C5 flash')
    p.add_argument('--case', action='append', type=parse_case, help='WIDTH:KHZ, repeat up to 12 cases')
    p.add_argument('--timeout', type=float, default=45)
    p.add_argument('--soak', type=float, default=3)
    p.add_argument('--poll-ms', type=int, choices=(0, 100), default=0,
                   help='0=normal SDIO interrupt, 100=diagnostic RX polling')
    p.add_argument('--init-ms', type=int, default=4000,
                   help='Hosted INIT wait, 4000..20000 in 200 ms steps')
    p.add_argument('--report', default='firmware/p4_gesture/test-results/c5_automated.json')
    args = p.parse_args(argv)
    matrix = args.case or MATRIX
    if not 1 <= len(matrix) <= 12 or not 15 <= args.timeout <= 90 or not 0 <= args.soak <= 30:
        p.error('bounded limits: 1..12 cases, timeout 15..90 s, soak 0..30 s')
    if not 4000 <= args.init_ms <= 20000 or args.init_ms % 200 or args.timeout < args.init_ms / 1000 + 12:
        p.error('init-ms must be 4000..20000 in 200 ms steps; timeout must allow init-ms/1000 + 12 s')
    config = meoave.load_config()
    build = meoave.project_dir(config) / meoave.cfg(config, 'build', 'hosted_dir', 'build_meoave_hosted')
    path = meoave.resolve(args.report)
    r = {'ok': False, 'completed': False, 'cases': [], 'visual': 'NOT_RETESTED',
         'scope': 'native P4 USB only; P4 software reset per case; one C5 reset per case; no C5 flash or GPIO6 drive; radar inactive required',
         'network_connectivity': 'NOT_TESTED', 'ota': 'NOT_TESTED',
         'started': datetime.now().astimezone().isoformat()}
    try:
        if args.build:
            meoave.build(config, 'hosted')
        r['image'] = usb.image_identity(build)
        if args.install:
            r['install'] = {}
            usb.flash_once(build, report=r['install'])
        sha = r['image']['elf_sha']
        r['baseline'] = usb.check_application(sha, 10)
        # This profile's entry point explicitly keeps audio/radar disabled.
        with usb.open_native(r['baseline']['port'], usb.APP_PIDS) as stream:
            stream.write(b'UI\n'); stream.flush()
            end = time.monotonic() + 4
            pending = b''
            while time.monotonic() < end:
                pending += stream.read(4096)
                lines = pending.decode('ascii', errors='replace').splitlines()
                ui = next((x for x in lines if x.startswith('MEO:UI ') and 'radar=0' in x), None)
                if ui:
                    r['ui_baseline'] = ui
                    break
                pending = pending[-8192:]
            else:
                raise RuntimeError('Radar inactive not confirmed: refusing shared GPIO54 reset')
        for width, khz in matrix:
            print(f'CASE {width}-bit {khz} kHz', flush=True)
            case = run_case(sha, width, khz, args.timeout, args.soak, args.poll_ms, args.init_ms)
            r['cases'].append(case)
            usb.write_report(path, r)  # checkpoint after every case
            print(f"  {'PASS' if case['ok'] else 'FAIL'} {case.get('terminal', {}).get('state', case.get('error'))} {case['evidence']}", flush=True)
            if not case['completed']:
                raise RuntimeError('Case did not finish/health guard failed; matrix stopped without recovery retry')
        r['completed'] = True
        # Leave display/USB in a clean IDLE boot, not an active failed transport.
        r['final_restart'] = {}
        usb.restart_once(sha, 30, r['final_restart'])
        r['postflight'] = {}
        postflight(sha, r['postflight'])
        r['ok'] = all(case['ok'] for case in r['cases'])
        r['passing_cases'] = [[c['width'], c['khz']] for c in r['cases'] if c['ok']]
        return 0 if r['ok'] else 2
    except Exception as exc:
        r['error'] = f'{type(exc).__name__}: {exc}'
        print(r['error'], flush=True)
        return 1
    finally:
        r['finished'] = datetime.now().astimezone().isoformat()
        usb.write_report(path, r)
        print('REPORT ' + str(path), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
