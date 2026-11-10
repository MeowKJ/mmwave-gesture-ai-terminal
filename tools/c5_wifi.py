#!/usr/bin/env python3
"""Real device Wi-Fi acceptance through P4 UART -> Hosted SDIO -> C5.

Password from --password-env or an interactive hidden prompt, NEVER an argv
value/file. NET_JOIN is not echoed or saved. C5 uses Wi-Fi RAM storage only.
--cycles repeats DNS/verified HTTPS, not firmware flashing or hidden reconnects.
"""
import argparse
from datetime import datetime
import getpass
import json
import os
from pathlib import Path
import time

import c5_maintenance as maintenance
import meoave


def credentials(ssid, password):
    s, p = ssid.encode('utf-8'), password.encode('utf-8')
    if not 1 <= len(s) <= 32 or not 8 <= len(p) <= 63 or b'\0' in s+p:
        raise ValueError('SSID must be 1..32 UTF-8 bytes; WPA password 8..63 bytes, no NUL')
    return s.hex(), p.hex()


def validate_config(config):
    if config.get('ESP_WIFI_NVS_ENABLED') or config.get('WIFI_RMT_NVS_ENABLED'):
        raise RuntimeError('Wi-Fi NVS is enabled in this P4 build; refusing credential transfer')
    if not config.get('MBEDTLS_CERTIFICATE_BUNDLE') or not config.get('MBEDTLS_HAVE_TIME_DATE'):
        raise RuntimeError('Verified CA bundle / certificate validity-date checks are required')


def redact(value, password):
    if isinstance(value, str):
        variants = (password, password.encode('utf-8').hex(), password.encode('utf-8').hex().upper())
        for secret in sorted(set(variants), key=len, reverse=True):
            if secret: value = value.replace(secret, '[REDACTED]')
        return value
    if isinstance(value, dict): return {k: redact(v, password) for k, v in value.items()}
    if isinstance(value, list): return [redact(v, password) for v in value]
    return value


def record(lines, prefix):
    found = [maintenance.fields(line, prefix) for line in lines if line.startswith(prefix+' ')]
    if len(found) != 1: raise RuntimeError(f'Expected exactly one {prefix} live result')
    return found[0]


def probe_result(link, target):
    error = None
    try:
        lines, _ = link.command(f'NET_PROBE {int(time.time())} {target}', timeout=100)
    except maintenance.MaintenanceCommandError as exc:
        if exc.op != 'NET_PROBE': raise
        lines, error = exc.lines, str(exc)
    # Explicit target/field checks, not merely a cached "connected" flag.
    dns = [maintenance.fields(s, 'MEO:NET_DNS') for s in lines if s.startswith('MEO:NET_DNS ')]
    https = [maintenance.fields(s, 'MEO:NET_HTTPS') for s in lines if s.startswith('MEO:NET_HTTPS ')]
    passed = (not error and len(dns) == len(https) == 1 and
              dns[0].get('target') == https[0].get('target') == target and
              int(dns[0]['error'], 0) == 0 and dns[0]['address'] not in ('none', '0.0.0.0') and
              int(https[0]['error'], 0) == 0 and 200 <= int(https[0]['status']) < 300 and
              int(https[0]['body_bytes']) > 0 and https[0].get('complete') == '1' and
              https[0].get('cert_verified') == '1')
    return dict(target=target, ok=bool(passed), dns=dns, https=https, error=error)


def same_c5(before, after):
    if any(before[k] != after[k] for k in ('elf_sha','boot','slot','boot_slot','ota_state')):
        raise RuntimeError('C5 changed/restarted during Wi-Fi testing')
    if after['ota_state'] != 2 or after['pin_mode'] != 0 or after['lease_ms'] != 0:
        raise RuntimeError('C5 must remain VALID with shared pin high-Z')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True, help='UART physically selected to P4')
    p.add_argument('--ssid', required=True)
    p.add_argument('--password-env', default='MEOAVE_WIFI_PASSWORD', help='environment variable NAME; never a password value')
    p.add_argument('--p4-build', type=Path, default=maintenance.DEFAULT_BUILD)
    p.add_argument('--c5-image', type=Path, default=maintenance.DEFAULT_IMAGE)
    p.add_argument('--recover-baseline', action='store_true', help='one explicit C5 EN reset before testing; may roll back an unconfirmed candidate')
    p.add_argument('--cycles', type=int, default=3)
    p.add_argument('--report', type=Path, default=meoave.ROOT/'firmware/p4_gesture/test-results/c5_wifi.json')
    a = p.parse_args(argv)
    if not 1 <= a.cycles <= 10: p.error('cycles must be 1..10')
    report = dict(ok=False, started=datetime.now().astimezone().isoformat(),
                  scope='real P4 TCP/IP over SDIO/C5 Wi-Fi; not PC network test',
                  c5_firmware_written=False, credentials_persisted=False, c5_straps_changed=False)
    stream, link, baseline, password = None, None, None, ''
    network_attempted = False
    try:
        import serial
        password = os.environ.get(a.password_env)
        if password is None: password = getpass.getpass('Wi-Fi password (not saved): ')
        ssid_hex, password_hex = credentials(a.ssid, password)
        p4 = maintenance.image_info((a.p4_build/'p4_c5_bringup.bin').read_bytes(), 'esp32p4')
        image = a.c5_image.read_bytes()
        maintenance.validate_candidate_mode(image, 'update')
        c5 = maintenance.image_info(image)
        config = json.loads((a.p4_build/'config/sdkconfig.json').read_text(encoding='utf-8'))
        validate_config(config)
        baud = meoave.console_uart_baud(a.p4_build)
        report['uart_baud'] = baud
        stream = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=3)
        stream.dtr = False; stream.rts = False; stream.port = a.port; stream.open()
        link = maintenance.Link(stream, p4['elf_sha'], report)
        baseline = maintenance.recover_baseline(link) if a.recover_baseline else link.attach()
        if baseline['elf_sha'] != c5['elf_sha'] or baseline['ota_state'] != 2 or baseline['pin_mode'] != 0:
            raise RuntimeError('Expected normal VALID C5 baseline was not observed')
        report['initial_c5'] = baseline
        network_attempted = True
        lines, _ = link.command('NET_SCAN '+ssid_hex, timeout=100)
        report['scan'] = record(lines, 'MEO:NET_SCAN')
        report['access_points'] = [maintenance.fields(s, 'MEO:NET_AP') for s in lines if s.startswith('MEO:NET_AP ')]
        if int(report['scan']['error'], 0) or int(report['scan']['matches']) < 1:
            raise RuntimeError('Target SSID was not found by C5 radio scan')
        print('Wi-Fi scan: target found', flush=True)
        lines, _ = link.command('NET_JOIN '+ssid_hex+' '+password_hex, timeout=60)
        report['join'] = record(lines, 'MEO:NET_JOIN')
        report['ip'] = record(lines, 'MEO:NET_IP')
        if int(report['join']['error'], 0) or report['join'].get('storage') != 'RAM':
            raise RuntimeError('Wi-Fi join did not prove RAM-only configuration')
        print('Wi-Fi associated; DHCP address received', flush=True)
        for cycle in range(a.cycles):
            results = [probe_result(link, target) for target in ('example', 'espressif')]
            report.setdefault('probe_cycles', []).append(results)
            for result in results:
                print(f"DNS/HTTPS cycle {cycle+1}/{a.cycles}: {result['target']} {'PASS' if result['ok'] else 'FAIL'}", flush=True)
            if not any(r['ok'] for r in results):
                raise RuntimeError('No endpoint passed real device DNS + verified complete HTTPS in this cycle')
            same_c5(baseline, link.inspect())
        report['all_targets_ok'] = all(r['ok'] for cycle in report['probe_cycles'] for r in cycle)
        report['ok'] = True
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print(redact(report['error'], password or ''), flush=True)
    finally:
        if network_attempted and link and 'p4_fault_tail' not in report:
            try:
                lines, _ = link.command('NET_STOP', timeout=25)
                report['cleanup'] = record(lines, 'MEO:NET_STOP')
                if any(report['cleanup'].get(k) != '1' for k in ('cleared','stopped','deinitialized')):
                    raise RuntimeError('Wi-Fi RAM configuration cleanup not verified')
                report['final_c5'] = link.inspect()
                same_c5(baseline, report['final_c5'])
            except Exception as exc:
                report['ok'] = False
                report['cleanup_error'] = f'{type(exc).__name__}: {exc}'
                print(redact(report['cleanup_error'], password or ''), flush=True)
        if stream: stream.close()
        report['finished'] = datetime.now().astimezone().isoformat()
        report = redact(report, password or '')
        a.report.parent.mkdir(parents=True, exist_ok=True)
        temp = a.report.with_suffix(a.report.suffix+'.tmp')
        temp.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        temp.replace(a.report)
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
