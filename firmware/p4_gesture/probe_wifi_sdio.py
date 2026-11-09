#!/usr/bin/env python3
"""One explicit CMD5(arg=0) diagnostic via P4 native USB, never C5 reset/flash.

Requires the matching product firmware. A completed query/SDIO R4 is NOT a
Hosted handshake, chip identity, a working Wi-Fi driver or a damage diagnosis.
The query temporarily configures P4 SDMMC and GPIO14/18/19; it is not a passive
electrical measurement. Never retransmit PROBE automatically.
"""
import argparse
from datetime import datetime
from pathlib import Path
import re
import secrets
import time

import flash_usb as usb

STATUS = re.compile(
    r'MEO:WIFI token=(?P<token>[a-zA-Z0-9]{1,16}) probe=(?P<probe>[a-zA-Z0-9]{1,16}) '
    r'state=(?P<state>IDLE|QUEUED|RUNNING|DONE) '
    r'result=(?P<result>NOT_RUN|PENDING|SDIO_R4|INVALID_R4|NO_RESPONSE|TRANSPORT_ERROR|HOST_BUSY|HOST_ERROR|TASK_ERROR) '
    r'error=0x(?P<error>[0-9a-f]+) ocr=0x(?P<ocr>[0-9a-f]{8}) '
    r'funcs=(?P<funcs>[0-7]) ready=(?P<ready>[01]) elapsed_ms=(?P<elapsed_ms>\d+) '
    r'cleanup=0x(?P<cleanup>[0-9a-f]+) blocked=(?P<blocked>[01]) '
    r'hosted=NOT_VERIFIED wifi=UNAVAILABLE reset=0 idle=0x(?P<idle>[0-9a-f]+)')
ACK = re.compile(r'MEO:WIFI_ACCEPT token=([a-zA-Z0-9]{1,16}) accepted=([01])')


def parse_status(line):
    match = STATUS.fullmatch(line)
    if not match:
        raise ValueError('Incomplete or invalid Wi-Fi diagnostic record')
    s = match.groupdict()
    for name in ('error', 'ocr', 'cleanup', 'idle'):
        s[name] = int(s[name], 16)
    for name in ('funcs', 'ready', 'elapsed_ms', 'blocked'):
        s[name] = int(s[name])
    if s['funcs'] != ((s['ocr'] >> 28) & 7) or s['ready'] != (s['ocr'] >> 31):
        raise ValueError('Inconsistent R4 fields')
    if s['state'] == 'DONE' and s['result'] in ('NOT_RUN', 'PENDING'):
        raise ValueError('Completed probe has no result')
    if s['result'] == 'SDIO_R4' and (s['error'] or not s['funcs'] or
                                    not (s['ocr'] & 0xff8000) or s['ocr'] == 0xffffffff):
        raise ValueError('SDIO_R4 claimed without a plausible successful response')
    return s


def collect_probe(stream, token, report, timeout=8, clock=time.monotonic):
    if not re.fullmatch(r'[a-zA-Z0-9]{1,16}', token):
        raise ValueError('Invalid probe nonce')
    pending = b''
    deadline, next_status = clock() + timeout, 0
    # Exactly one request. If ACK is lost, STATUS with the same probe nonce
    # recovers its outcome without initiating a second bus operation.
    stream.write(f'WIFI PROBE {token}\n'.encode('ascii'))
    stream.flush()
    while clock() < deadline:
        now = clock()
        if now >= next_status:
            stream.write(f'WIFI STATUS {token}\nUI\n'.encode('ascii'))
            stream.flush()
            next_status = now + .3
        pending += stream.read(4096)
        while b'\n' in pending:
            raw, pending = pending.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace').rstrip('\r')
            if len(report['lines']) < 2000:
                report['lines'].append(line)
            if line.startswith('MEO:WIFI_ACCEPT '):
                ack = ACK.fullmatch(line)
                if not ack:
                    raise ValueError('Malformed Wi-Fi ACK')
                if ack[1] == token:
                    report['accepted'] = ack[2] == '1'
            elif line.startswith('MEO:WIFI '):
                s = parse_status(line)
                if s['token'] == token:
                    if report.get('accepted') is False and s['probe'] != token:
                        raise RuntimeError('Probe rejected: existing task or blocked controller')
                    if s['probe'] == token and s['state'] == 'DONE':
                        return s
        if len(pending) > 4096:
            raise ValueError('Unbounded/no-newline native USB output')
    raise TimeoutError('No fresh complete probe result; not retrying, resetting or flashing')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build_meoave')
    parser.add_argument('--report', default='test-results/wifi_sdio_probe.json')
    # Manual C5 boot-mode entry (straps held low by hand) cannot survive an
    # unexplained P4 restart being treated as a clean baseline, but skipping the
    # baseline check must never be silent: --no-reset records clean_start=False
    # and still refuses if the P4 reboots mid-probe.
    parser.add_argument('--no-reset', action='store_true',
                        help='Probe without requiring a preceding program restart '
                             '(use when the C5 was manually reset into download boot)')
    args = parser.parse_args()
    report = {'ok': False, 'probe_completed': False, 'sdio_response_observed': False,
              'hosted': 'NOT_VERIFIED', 'wifi': 'UNAVAILABLE', 'visual': 'NOT_VERIFIED',
              'c5_reset': 'never', 'clean_start': None,
              'started': datetime.now().astimezone().isoformat(), 'lines': [],
              'scope': 'one CMD5(arg=0); P4 native USB only; no C5 reset/flash'}
    try:
        build = Path(args.build_dir)
        if not build.is_absolute():
            build = Path(__file__).resolve().parent / build
        expected = usb.image_identity(build)['elf_sha']
        report['before'] = before = usb.check_application(expected, 15)
        if args.no_reset:
            report['clean_start'] = False
        else:
            usb.require_clean_reset(before, report)
            report['clean_start'] = True
        with usb.open_native(before['port'], usb.APP_PIDS) as stream:
            stream.reset_input_buffer()
            s = collect_probe(stream, secrets.token_hex(6), report)
        report.update(probe=s, probe_completed=True,
                      sdio_response_observed=s['result'] == 'SDIO_R4')
        report['after'] = after = usb.check_application(expected, 10)
        if not args.no_reset:
            usb.require_clean_reset(after, report)
        if after['boot_id'] != before['boot_id']:
            raise RuntimeError('Unexpected reboot during SDIO probe')
        report['fault'] = usb.read_fault_diagnostic(after['port'])
        if report['fault'].get('valid') != '0':
            raise RuntimeError('Fault diagnostic missing or fault recorded')
        if s['cleanup'] or s['blocked'] or s['result'] not in ('SDIO_R4', 'NO_RESPONSE'):
            raise RuntimeError('Probe/cleanup error: inspect report, no automatic recovery')
        report['ok'] = True  # Diagnostic/health only, never Wi-Fi acceptance.
        print(f"COMPLETE: {s['result']}, OCR=0x{s['ocr']:08x}; Hosted NOT_VERIFIED, Wi-Fi UNAVAILABLE")
        return 0
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print('FAIL: ' + report['error'])
        return 1
    finally:
        report['finished'] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == '__main__':
    raise SystemExit(main())
