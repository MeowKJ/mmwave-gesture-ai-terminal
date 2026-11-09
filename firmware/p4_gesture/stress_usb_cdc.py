#!/usr/bin/env python3
"""Bounded read-only native-USB record stress. No flash, restart, UART or fake touch."""
import argparse
from datetime import datetime
from pathlib import Path
import re
import secrets
import time

import flash_usb as usb
from check_meoave_ui import UI_PATTERN

RULES = {
    'ui': UI_PATTERN,
    'pong': re.compile(r'FLS2:PONG uptime_us=\d+'),
    'status': re.compile(r'FLS2:STATUS profile=meoave_light ready=[01] usb=1 rgb888=1 lanes=1 mbps=500 dpi_mhz=16 vsync=NOT_MEASURED'),
    'difference': re.compile(r'FLS2:DIFFERENCE dcs_id_read=SKIPPED_BY_EXISTING_LOCAL_PATCH panel_ack=NOT_VERIFIED'),
    'loop': re.compile(r'FLS2:LOOP enabled=0 round=\d+ phase=WAIT boot_auto=0 dcs_probe=EXCLUDED reboot_required=0'),
    'touch': re.compile(r'FLS2:TOUCH init=1 addr=0x15 sda=8 scl=9 hz=400000 probe=[01] id=0xB7 id_ok=\d+ id_err=0'),
    'touch_io': re.compile(r'FLS2:TOUCH_IO driver_ok=\d+ driver_err=0 probe_ok=\d+ probe_err=0 last=ESP_OK age_ms=-?\d+ driver_age_ms=-?\d+'),
    'touch_xy': re.compile(r'FLS2:TOUCH_XY n=\d+ x=\d+ y=\d+ frames=\d+ pressed=\d+ edges=\d+ invalid=\d+ age_ms=-?\d+'),
    'touch_raw': re.compile(r'FLS2:TOUCH_RAW(?: [0-9A-F]{2}){15}'),
    'info': usb.INFO_PATTERN,
}
LOG_PATTERN = re.compile(r'(?:\x1b\[[0-9;]*m)*[EWIDV] \(\d+\) .*')
MACHINE_MARKER = re.compile(r'MEO:UI |SYS:(?:INFO|POWER|FAULT|RESTART) |FLS2:(?:PONG|STATUS|DIFFERENCE|LOOP|TOUCH(?:_IO|_XY|_RAW)?) ')


def classify(line):
    for name, pattern in RULES.items():
        if pattern.fullmatch(line):
            return name
    if not line or (LOG_PATTERN.fullmatch(line) and not MACHINE_MARKER.search(line)):
        return 'log'
    return 'malformed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build_meoave')
    parser.add_argument('--expected-sha', help='full ELF SHA for a known older baseline')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--period', type=float, default=.1)
    parser.add_argument('--report', default='test-results/cdc_record_stress.json')
    args = parser.parse_args()
    if not 5 <= args.seconds <= 120 or not .05 <= args.period <= 1:
        parser.error('seconds must be 5..120; period must be .05..1 seconds')
    if args.expected_sha and not re.fullmatch(r'[0-9a-f]{64}', args.expected_sha):
        parser.error('expected-sha must be a complete lowercase 64-digit ELF SHA')
    report = {'ok': False, 'started': datetime.now().astimezone().isoformat(),
              'seconds': args.seconds, 'period': args.period, 'sent_batches': 0,
              'counts': {}, 'malformed': [], 'lines': [], 'visual': 'NOT_VERIFIED',
              'scope': 'record integrity/progress, not guaranteed delivery; native USB only; read-only commands'}
    try:
        build = Path(args.build_dir)
        if not build.is_absolute():
            build = Path(__file__).resolve().parent / build
        expected = args.expected_sha or usb.image_identity(build)['elf_sha']
        before = usb.check_application(expected, 15)
        report['before'] = before
        usb.require_clean_reset(before, report)
        issued, replies, frames, pongs = set(), set(), [], []
        with usb.open_native(before['port'], usb.APP_PIDS) as stream:
            stream.reset_input_buffer()
            pending = b''
            start, next_send = time.monotonic(), 0
            deadline = start + args.seconds
            # Drain for one additional second without stopping the board's logs.
            while time.monotonic() < deadline + 1:
                now = time.monotonic()
                if now < deadline and now >= next_send:
                    token = secrets.token_hex(6)
                    issued.add(token)
                    stream.write(f'SYS INFO {token}\nPING\nUI\nSTATUS\n'.encode('ascii'))
                    stream.flush()
                    report['sent_batches'] += 1
                    next_send = now + args.period
                pending += stream.read(4096)
                while b'\n' in pending:
                    raw, pending = pending.split(b'\n', 1)
                    line = raw.decode('ascii', errors='replace').rstrip('\r')
                    kind = classify(line)
                    report['counts'][kind] = report['counts'].get(kind, 0) + 1
                    if len(report['lines']) < 20000:
                        report['lines'].append(line)
                    if kind == 'malformed' and len(report['malformed']) < 50:
                        report['malformed'].append(line)
                    if kind == 'info':
                        fields = usb.INFO_PATTERN.fullmatch(line)
                        if fields[1] not in issued or fields[2] != expected or fields[5] != before['boot_id']:
                            raise RuntimeError('Unexpected identity/token/boot during stress')
                        replies.add(fields[1])
                    elif kind == 'ui':
                        frames.append(int(re.search(r' frames=(\d+)', line)[1]))
                    elif kind == 'pong':
                        pongs.append(int(line.split('=')[1]))
                if len(pending) > 4096:
                    raise RuntimeError('Unbounded/no-newline output during stress')
            # A partial record at a sample boundary is not itself corruption.
            report['trailing_bytes'] = len(pending)
        report['unique_info_replies'] = len(replies)
        report['ui_frame_delta'] = frames[-1] - frames[0] if len(frames) > 1 else 0
        report['pong_delta_us'] = pongs[-1] - pongs[0] if len(pongs) > 1 else 0
        after = usb.check_application(expected, 10)
        report['after'] = after
        usb.require_clean_reset(after, report)
        if after['boot_id'] != before['boot_id']:
            raise RuntimeError('Unexpected reboot during stress')
        if report['counts'].get('malformed', 0):
            raise RuntimeError('Malformed/truncated/concatenated records observed')
        if len(replies) < 3 or len(pongs) < 3 or report['ui_frame_delta'] <= 0 or report['counts'].get('status', 0) < 2:
            raise RuntimeError('Fresh replies / GUI progress / status not sufficiently observed')
        report['ok'] = True
        print('PASS: complete records, fresh identity replies, GUI progress, same boot; visual NOT_VERIFIED')
        print(report['counts'])
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
