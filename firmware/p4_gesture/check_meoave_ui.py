#!/usr/bin/env python3
"""Read-only product GUI/USB health, NOT physical display/touch acceptance."""
import argparse
from datetime import datetime
from pathlib import Path
import re
import time
import flash_usb as usb

UI_PATTERN = re.compile(
    r'MEO:UI page=(eyes|apps|settings|wifi|breathe|display|accessibility) frames=\d+ '
    r'rotation=180 brightness=\d+ reduced=[01] running=[01] mode=(GUIDED|RADAR) '
    r'elapsed_ms=\d+ wifi=UNAVAILABLE radar=[01] app_view=(GRID|LIST) eye=[0-6] eye_auto=[01]')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build_meoave')
    parser.add_argument('--report', default='test-results/meoave_ui_runtime.json')
    parser.add_argument('--duration', type=float, default=8, help='Bounded observation, 2..120 seconds')
    args = parser.parse_args()
    if not 2 <= args.duration <= 120:
        parser.error('--duration must be between 2 and 120 seconds')
    build = Path(args.build_dir)
    if not build.is_absolute():
        build = Path(__file__).resolve().parent / build
    report = {'ok': False, 'started': datetime.now().astimezone().isoformat(),
              'visual': 'NOT_VERIFIED', 'physical_touch_mapping': 'NOT_VERIFIED', 'lines': []}
    try:
        expected = usb.image_identity(build)['elf_sha']
        before = usb.check_application(expected, 15)
        report['before'] = before
        usb.require_clean_reset(before, report)
        samples, ready = [], False
        deadline, next_send = time.monotonic()+args.duration, 0
        pending = b''
        with usb.open_native(before['port'], usb.APP_PIDS) as stream:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now >= next_send:
                    # STATUS triggers several large diagnostic records. Request
                    # the compact GUI record only; periodic status proves ready.
                    stream.write(b'UI\n')
                    stream.flush()
                    next_send = now+.75
                pending += stream.read(4096)
                while b'\n' in pending:
                    raw, pending = pending.split(b'\n', 1)
                    line = raw.decode('ascii', errors='replace').rstrip('\r')
                    report['lines'].append(line)
                    if line.startswith('FLS2:STATUS profile=meoave_light ready=1 usb=1 '):
                        ready = True
                    if line.startswith('MEO:UI '):
                        if not UI_PATTERN.fullmatch(line):
                            report['discarded_incomplete_ui'] = report.get('discarded_incomplete_ui', 0)+1
                            continue
                        sample = dict(re.findall(r'(\w+)=(\S+)', line))
                        if sample.get('rotation') != '180':
                            raise RuntimeError('Unexpected display rotation configuration')
                        sample['frames'] = int(sample['frames'])
                        samples.append(sample)
                pending = pending[-4096:]
        if not ready or len(samples) < 2 or samples[-1]['frames'] <= samples[0]['frames']:
            raise RuntimeError('Product readiness / GUI tick progress not verified')
        after = usb.check_application(expected, 10)
        report['after'] = after
        usb.require_clean_reset(after, report)
        if after['boot_id'] != before['boot_id']:
            raise RuntimeError('Unexpected reboot during GUI observation')
        report.update(ok=True, samples=samples, frame_delta=samples[-1]['frames']-samples[0]['frames'],
                      expressions_seen=sorted({int(s['eye']) for s in samples}))
        print('PASS: Meoave identity, GUI tick progress, 180-degree config and power hold; visual NOT_VERIFIED')
        print(samples[-1])
        return 0
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print('FAIL: '+report['error'])
        return 1
    finally:
        report['finished'] = datetime.now().astimezone().isoformat()
        usb.write_report(args.report, report)


if __name__ == '__main__':
    raise SystemExit(main())
