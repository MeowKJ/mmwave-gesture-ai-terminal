#!/usr/bin/env python3
"""P4 UART -> Hosted SDIO app-only updates. Never changes boot straps.

Every state-changing command is sent once. An ambiguous WRITE/ACTIVATE stops
the run, not a retry loop. Initial C5 UART provisioning is a separate tool.
"""
import argparse
from datetime import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import secrets
import struct
import time

import meoave

DEFAULT_BUILD = meoave.ROOT / 'firmware/p4_gesture/bringup/build_maintenance'
DEFAULT_IMAGE = meoave.ROOT / 'firmware/c5_hosted/build_c5/network_adapter.bin'
FAULT_MARKER = b'FAULT_TEST pending image resets before Hosted'
HANG_MARKER = b'HANG_TEST pending image blocks before Hosted'


class MaintenanceCommandError(RuntimeError):
    """A matching, explicit P4 error reply, not a lost/corrupted UART record."""
    def __init__(self, op, reply, lines):
        self.op, self.reply, self.lines = op, reply, lines
        self.error = int(reply['error'], 0)
        super().__init__(f'{op} failed: {lines[-1]}; no automatic retry')


def validate_candidate_mode(data, mode):
    fault, hung = FAULT_MARKER in data, HANG_MARKER in data
    if mode == 'fault' and (not fault or hung):
        raise ValueError('fault requires a separate reset-before-Hosted failure image')
    if mode == 'hung' and (not hung or fault):
        raise ValueError('hung requires a separate block-before-Hosted failure image')
    if mode in ('update', 'unconfirmed') and (fault or hung):
        raise ValueError('Refusing failure-test image as a normal update')


def expect_unresponsive(link):
    """Only explicit C5 transport absence with responsive P4 is expected."""
    try:
        link.command('ATTACH', timeout=40)
    except MaintenanceCommandError as exc:
        absent = exc.error == 0x107 or (
            exc.error == 0xffffffff and any('ensure_slave_bus_ready failed' in s for s in exc.lines))
        if exc.op != 'ATTACH' or exc.reply.get('state') != 'FAILED' or not absent:
            raise
        return dict(error=exc.error, state=exc.reply['state'])
    raise RuntimeError('Hung candidate unexpectedly provided Hosted; injection not proven')


def image_info(data, chip='esp32c5'):
    """Reject merged, corrupted, wrong-chip and too-large images before UART."""
    from esptool.bin_image import ESP32C5FirmwareImage, ESP32P4FirmwareImage
    expected = 23 if chip == 'esp32c5' else 18
    if len(data) < 288 or data[0] != 0xe9 or struct.unpack_from('<H', data, 12)[0] != expected:
        raise ValueError('Not the requested single app image (no merged/bootloader images)')
    if data[32:36] != b'\x32\x54\xcd\xab':
        raise ValueError('Missing app descriptor')
    image = (ESP32C5FirmwareImage if chip == 'esp32c5' else ESP32P4FirmwareImage)(io.BytesIO(data))
    if not image.append_digest or image.checksum != image.calculate_checksum() or image.stored_digest != image.calc_digest:
        raise ValueError('App checksum/SHA256 verification failed')
    # Only exact IDF app binaries are accepted, not an app followed by Flash data.
    if len(data) != image.data_length + 32:
        raise ValueError('App contains trailing Flash data')
    if chip == 'esp32c5' and len(data) > 0x1e0000:
        raise ValueError('C5 app exceeds inactive slot capacity')
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                elf_sha=data[176:208].hex(), chip=chip,
                min_revision=struct.unpack_from('<H', data, 15)[0],
                max_revision=struct.unpack_from('<H', data, 17)[0])


def fields(line, prefix):
    if not line.startswith(prefix + ' '):
        raise ValueError('Unexpected record')
    result = {}
    for item in line[len(prefix)+1:].split():
        key, value = item.split('=', 1)
        if key in result:
            raise ValueError('Duplicate record field')
        result[key] = value
    return result


def c5_record(lines, op=1):
    records = [fields(s, 'MEO:C5_MAINT') for s in lines if s.startswith('MEO:C5_MAINT ')]
    if len(records) != 1:
        raise RuntimeError('Expected one live C5 maintenance response')
    r = records[0]
    for key in ('op', 'seq', 'received', 'error', 'reset', 'slot', 'boot_slot', 'ota_state',
                'flags', 'pin_mode', 'lease_ms', 'pin_level', 'uptime_ms'):
        if key in r:
            r[key] = int(r[key], 0)
    if r['op'] != op or r['received'] != 1 or r['error']:
        raise RuntimeError('C5 response failed or timed out')
    if not re.fullmatch('[0-9a-f]{64}', r.get('elf_sha', '')) or r['slot'] not in (0x20000, 0x200000):
        raise RuntimeError('Invalid C5 identity/slot')
    if not re.fullmatch('[0-9a-f]{8}', r.get('boot', '')):
        raise RuntimeError('Missing C5 boot nonce')
    return r


class Link:
    def __init__(self, stream, expected_elf, report):
        self.stream, self.expected_elf, self.report = stream, expected_elf, report
        self.pending = b''

    def line(self, deadline):
        while time.monotonic() < deadline:
            if b'\n' in self.pending:
                raw, self.pending = self.pending.split(b'\n', 1)
                text = raw.decode('ascii', errors='replace').rstrip('\r')
                self.report.setdefault('uart_lines', []).append(text)
                if 'Guru Meditation' in text or 'task_wdt:' in text:
                    # Preserve the register dump/backtrace, not only the panic
                    # header. No commands/resets are issued during capture.
                    raw = self.pending
                    self.pending = b''
                    end = time.monotonic() + 3
                    while time.monotonic() < end:
                        waiting = getattr(self.stream, 'in_waiting', 4096)
                        raw += self.stream.read(max(1, min(waiting, 4096)))
                        if len(raw) > 65536:
                            break
                    self.report['p4_fault_tail'] = raw.decode('ascii', errors='replace')
                    raise RuntimeError('P4 panic/watchdog; halted')
                return text
            # Do not wait for a 4KB read timeout after every short stop/wait ACK.
            # A real serial stream exposes in_waiting; simple test streams may not.
            waiting = getattr(self.stream, 'in_waiting', 4096)
            self.pending += self.stream.read(max(1, min(waiting, 4096)))
            if len(self.pending) > 32768:
                raise RuntimeError('Unbounded UART record')
        raise TimeoutError('UART response timed out; command NOT retried')

    def send(self, command):
        data = (command + '\n').encode('ascii')
        if self.stream.write(data) != len(data):
            raise RuntimeError('Short UART write; command outcome unknown')
        self.stream.flush()

    def identify(self, timeout=5):
        self.send('INFO')
        deadline = time.monotonic() + timeout
        identity = None
        while True:
            line = self.line(deadline)
            if line.startswith('MEO:BRINGUP '):
                identity = fields(line, 'MEO:BRINGUP')
            if line.startswith('MEO:MAINT_STATUS ') and identity:
                if identity.get('project') != 'p4_c5_bringup' or identity.get('profile') != 'maintenance' or identity.get('elf_sha') != self.expected_elf:
                    raise RuntimeError('P4 image/profile identity mismatch; refusing commands')
                self.report.setdefault('p4_identities', []).append(identity)
                return fields(line, 'MEO:MAINT_STATUS')

    def command(self, command, timeout=12, token=None, offset=None):
        op = command.split()[0]
        self.send(command)
        deadline, lines = time.monotonic() + timeout, []
        while True:
            line = self.line(deadline)
            lines.append(line)
            if line.startswith('MEO:MAINT_REPLY '):
                r = fields(line, 'MEO:MAINT_REPLY')
                if r.get('op') != op:
                    raise RuntimeError('Unexpected reply; not reusing stale result')
                if token is not None and r.get('token') != token:
                    raise RuntimeError('Transaction token mismatch')
                if int(r['error'], 0):
                    raise MaintenanceCommandError(op, r, lines)
                if offset is not None and int(r['offset']) != offset:
                    raise RuntimeError(f'{op} wrong offset: {line}; no automatic retry')
                return lines, r

    def reset_p4(self):
        self.send('P4RESET')
        deadline = time.monotonic() + 5
        while not self.line(deadline).startswith('MEO:P4RESET accepted=1 c5_reset=0'):
            pass
        time.sleep(1.5)
        state = self.identify(12)
        if state['state'] != 'IDLE':
            raise RuntimeError('P4 did not restart in IDLE')

    def attach(self, reset_c5=False):
        state = self.identify()
        if state['state'] == 'IDLE':
            self.command('CONNECT' if reset_c5 else 'ATTACH', timeout=40)
        elif state['state'] != 'READY':
            raise RuntimeError(f"P4 state {state['state']}; use explicit recovery, not retry")
        return self.inspect()

    def inspect(self):
        lines, _ = self.command('VERIFY')
        ids = [fields(s, 'MEO:HOSTED_ID') for s in lines if s.startswith('MEO:HOSTED_ID ')]
        if len(ids) != 1 or ids[0].get('chip') != 'esp32c5' or ids[0].get('chip_id') != '23' or int(ids[0]['error'], 0):
            raise RuntimeError('Live C5 chip/version RPC failed')
        r = c5_record(lines)
        self.report.setdefault('c5_health', []).append(r)
        return r

    def confirm(self, expected, slot=None):
        snapshots = []
        for _ in range(3):
            snapshots.append(self.inspect())
            time.sleep(0.8)
        r = snapshots[-1]
        if any(s['elf_sha'] != expected or s['boot'] != r['boot'] or s['slot'] != r['slot'] for s in snapshots):
            raise RuntimeError('C5 image changed/restarted during health check')
        if r['boot_slot'] != r['slot'] or (slot is not None and r['slot'] != slot) or r['pin_mode'] != 0 or r['uptime_ms'] < 2000:
            raise RuntimeError('C5 health/slot/pin checks failed; NOT confirmed')
        lines, _ = self.command('CONFIRM ' + expected)
        confirmed = c5_record(lines, 2)
        if confirmed['ota_state'] != 2 or confirmed['elf_sha'] != expected or confirmed['boot'] != r['boot']:
            raise RuntimeError('C5 did not report VALID on this boot')
        self.report.setdefault('confirmed', []).append(confirmed)
        return confirmed


def update(link, data, meta, mode='update'):
    old = link.inspect()
    if old['ota_state'] != 2 or not old['flags'] & 2 or old['pin_mode'] != 0 or old['slot'] != old['boot_slot']:
        raise RuntimeError('Baseline must be VALID, rollback-capable and high-Z')
    token = secrets.token_hex(6)
    transaction = dict(token=token, mode=mode, source=old, image=meta, activated=False)
    link.report.setdefault('transactions', []).append(transaction)
    link.command(f"OTA_BEGIN {token} {len(data)} {meta['sha256']} {meta['elf_sha']}", timeout=30, token=token, offset=0)
    limit = min(4096, len(data)) if mode == 'interrupted' else len(data)
    for offset in range(0, limit, 512):
        block = data[offset:min(offset+512, limit)]
        link.command(f'OTA_WRITE {token} {offset} {block.hex()}', token=token, offset=offset+len(block))
        transaction['acknowledged_bytes'] = offset + len(block)
        if offset % 65536 == 0:
            print(f"SDIO {offset+len(block)}/{len(data)}", flush=True)
    if mode == 'interrupted':
        # No END or ACTIVATE. C5 EN reset abandons its open flash session.
        link.reset_p4()
        link.command('CONNECT', timeout=40)
        current = link.inspect()
        if current['elf_sha'] != old['elf_sha'] or current['slot'] != old['slot'] or current['ota_state'] != 2:
            raise RuntimeError('Interrupted upload did not preserve old VALID app')
        transaction['interrupted_upload_preserved_baseline'] = True
        return
    link.command('OTA_END ' + token, timeout=30, token=token, offset=len(data))
    transaction['activation_requested'] = True  # Lost ACK is an ambiguous activation.
    link.command('OTA_ACTIVATE ' + token, token=token)
    transaction['activated'] = True
    # ACTIVATE schedules C5 reboot in 2 seconds. Never pulse EN on first attach!
    time.sleep(4)
    link.reset_p4()
    if mode == 'fault':
        time.sleep(3)
    if mode == 'hung':
        # UART timeout, panic, stale reply and arbitrary errors must NEVER
        # count as successful fault injection. Wait past the normal 90s
        # self-rollback deadline, then prove it is still absent without EN.
        transaction['unresponsive_attach'] = [expect_unresponsive(link)]
        time.sleep(95)
        transaction['unresponsive_wait_seconds'] = 95
        link.reset_p4()
        transaction['unresponsive_attach'].append(expect_unresponsive(link))
        restored = recover_previous(link, transaction)
        if restored['boot'] == old['boot'] or restored['pin_mode'] != 0:
            raise RuntimeError('Hung recovery did not show a fresh high-Z baseline')
        transaction['hung_candidate_en_rollback_verified'] = True
        return
    link.command('ATTACH', timeout=40)
    current = link.inspect()
    if mode == 'fault':
        if current['elf_sha'] != old['elf_sha'] or current['slot'] != old['slot'] or current['ota_state'] != 2 or current['boot'] == old['boot']:
            raise RuntimeError('Fault-image rollback NOT proven')
        transaction['bootloader_fault_rollback_verified'] = True
        return
    if current['elf_sha'] != meta['elf_sha'] or current['slot'] == old['slot'] or current['ota_state'] != 1 or current['boot'] == old['boot']:
        raise RuntimeError('New exact image in PENDING_VERIFY on other slot not observed')
    transaction['pending_observed'] = current
    if mode == 'unconfirmed':
        # Intentionally do not confirm; the candidate should reboot at uptime90s.
        time.sleep(max(0, (95000-current['uptime_ms'])/1000))
        link.reset_p4()
        link.command('ATTACH', timeout=40)
        restored = link.inspect()
        if restored['elf_sha'] != old['elf_sha'] or restored['slot'] != old['slot'] or restored['ota_state'] != 2:
            raise RuntimeError('Unconfirmed-image timeout rollback NOT proven')
        transaction['timeout_rollback_verified'] = True
        return
    transaction['confirmed'] = link.confirm(meta['elf_sha'], current['slot'])


def pin_test(link):
    """Weak pulls only. Readback is digital, NOT a voltage measurement."""
    results = []
    for name, mode in (('UP', 1), ('DOWN', 2)):
        lines, _ = link.command(f'PIN {name} 500')
        r = c5_record(lines, 3)
        if r['pin_mode'] != mode or not 0 < r['lease_ms'] <= 500:
            raise RuntimeError('Pin lease not applied')
        p4 = [fields(s, 'MEO:P4_PIN') for s in lines if s.startswith('MEO:P4_PIN ')]
        if len(p4) != 1:
            raise RuntimeError('No P4 digital input readback')
        expected_level = 1 if mode == 1 else 0
        if int(p4[0]['level']) != expected_level or r['pin_level'] != expected_level:
            raise RuntimeError('Weak pull digital levels disagree; no strong drive attempted')
        time.sleep(0.7)
        released = link.inspect()
        if released['pin_mode'] != 0 or released['lease_ms'] != 0:
            raise RuntimeError('Pin lease did not expire to high-Z')
        results.append(dict(mode=name, applied=r, p4=p4[0], expired=released))
    link.report['pin_weak_pull_test'] = results
    link.report['pin_scope'] = 'weak pulls/high-Z, digital input readbacks; no analog measurement or product audio validation'


def reset_soak(link, baseline, cycles):
    """Reset P4 only; every live RPC must retain the same C5 boot and VALID slot."""
    if baseline['ota_state'] != 2 or baseline['slot'] != baseline['boot_slot']:
        raise RuntimeError('Reset soak requires a confirmed C5 baseline')
    for i in range(cycles):
        link.reset_p4()
        current = link.attach(reset_c5=False)
        if any(current[k] != baseline[k] for k in ('boot', 'elf_sha', 'slot', 'boot_slot', 'ota_state')):
            raise RuntimeError('C5 restarted/changed during P4-only reset soak')
        link.report.setdefault('reset_cycles', []).append(current)
        print(f'P4 reset {i+1}/{cycles}: C5 boot={current["boot"]} VALID, no C5 reset', flush=True)


def recover_baseline(link):
    """Explicit, single pre-upload EN reset; never called on a new candidate."""
    recovery = link.report['baseline_recovery'] = dict(attempted=True, max_c5_en_resets=1, ok=False)
    link.identify()
    link.reset_p4()
    link.command('CONNECT', timeout=40)
    current = link.inspect()
    recovery['c5'] = current
    if current['ota_state'] != 2 or current['boot_slot'] != current['slot'] or current['pin_mode'] != 0:
        raise RuntimeError('No confirmed high-Z baseline after single pre-upload recovery')
    recovery['ok'] = True
    return current


def recover_previous(link, transaction):
    """One EN recovery total, including failed recovery; never resends OTA."""
    if link.report.get('automatic_recovery', {}).get('attempted'):
        raise RuntimeError('Single automatic recovery already attempted; no further reset')
    recovery = link.report['automatic_recovery'] = dict(attempted=True, max_c5_en_resets=1, ok=False)
    try:
        time.sleep(4)
        link.reset_p4()
        link.command('CONNECT', timeout=40)
        restored = link.inspect()
        old = transaction['source']
        recovery['c5'] = restored
        if (restored['elf_sha'] != old['elf_sha'] or restored['slot'] != old['slot'] or
                restored['boot_slot'] != restored['slot'] or restored['ota_state'] != 2 or restored['pin_mode'] != 0):
            raise RuntimeError('Previous high-Z VALID app not observed after single recovery reset')
        recovery['ok'] = True
        return restored
    except Exception as exc:
        recovery['error'] = f'{type(exc).__name__}: {exc}'
        raise


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('check', 'confirm', 'update', 'interrupted', 'unconfirmed', 'fault', 'hung', 'pins', 'recover', 'reset-soak'))
    p.add_argument('--port', required=True, help='UART must be physically routed to P4')
    p.add_argument('--p4-build', type=Path, default=DEFAULT_BUILD)
    p.add_argument('--image', type=Path, default=DEFAULT_IMAGE)
    p.add_argument('--cycles', type=int, default=1)
    p.add_argument('--recover-baseline', action='store_true',
                   help='before uploading only, reset P4 then C5 EN once and require a VALID baseline; may roll back a previous unconfirmed candidate')
    p.add_argument('--report', type=Path, default=meoave.ROOT/'firmware/p4_gesture/test-results/c5_maintenance.json')
    a = p.parse_args(argv)
    limit = 100 if a.action == 'reset-soak' else 10
    if not 1 <= a.cycles <= limit or (a.cycles != 1 and a.action not in ('update', 'reset-soak')):
        p.error('cycles1..10 for update, 1..100 for reset-soak')
    if a.recover_baseline and a.action not in ('update', 'interrupted', 'unconfirmed', 'fault', 'hung'):
        p.error('--recover-baseline is only for explicit upload actions, not a read-only check')
    report = dict(ok=False, action=a.action, started=datetime.now().astimezone().isoformat(),
                  c5_straps_changed=False, c5_bootloader_written=False)
    stream = None
    try:
        import serial
        p4 = image_info((a.p4_build/'p4_c5_bringup.bin').read_bytes(), 'esp32p4')
        data = a.image.read_bytes() if a.action in ('update', 'interrupted', 'unconfirmed', 'fault', 'hung') else None
        meta = image_info(data) if data else None
        if meta: report['candidate'] = meta
        if data: validate_candidate_mode(data, a.action)
        baud = meoave.console_uart_baud(a.p4_build)
        report['uart_baud'] = baud
        stream = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=3)
        stream.dtr=False; stream.rts=False; stream.port=a.port; stream.open()
        report['serial_control'] = dict(dtr=False, rts=False)
        link = Link(stream, p4['elf_sha'], report)
        if a.action == 'recover':
            report['recovered'] = recover_baseline(link)
        else:
            current = recover_baseline(link) if a.recover_baseline else link.attach()
            report['initial'] = current
            if a.action == 'confirm': link.confirm(current['elf_sha'])
            elif a.action == 'pins': pin_test(link)
            elif a.action == 'reset-soak': reset_soak(link, current, a.cycles)
            elif data:
                for i in range(a.cycles):
                    print(f'{a.action} cycle {i+1}/{a.cycles}', flush=True)
                    update(link, data, meta, a.action)
        report['ok'] = True
        return 0
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print(report['error'], flush=True)
        transactions = report.get('transactions', [])
        if (transactions and transactions[-1].get('activation_requested') and stream and
                not report.get('automatic_recovery', {}).get('attempted') and 'p4_fault_tail' not in report):
            # Recovery is a new, bounded operation, never a resend of an
            # ambiguous WRITE/ACTIVATE. EN reset lets the bootloader reject a
            # hung PENDING image even when its SDIO service is completely dead.
            try:
                recover_previous(link, transactions[-1])
            except Exception:
                print('Automatic recovery failed; no further resets/writes', flush=True)
        return 1
    finally:
        if stream: stream.close()
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
