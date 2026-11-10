#!/usr/bin/env python3
"""One-time C5 UART bootstrap, with full backup and bounded writes.

Must route UART to C5 first. Keeps original ota_0, partition table, NVS, keys,
certificates and factory calibration. Installs maintenance app in ota_1 plus a
rollback-enabled bootloader. Subsequent app updates use c5_maintenance.py.
"""
import argparse
import binascii
from datetime import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import struct

import meoave
from c5_maintenance import image_info

SIZE = 0x400000
BUILD = meoave.ROOT / 'firmware/c5_hosted/build_c5'


def read_bounded(esp, offset, size, block_size=1024):
    """Official stub READ_FLASH framing, one in-flight block instead of64.

    This changes read flow control only, never Flash geometry. Any short frame,
    excess byte or remote MD5 mismatch stops before data is trusted.
    """
    # pyserial on Windows re-applies SetCommState when timeout changes. Do this
    # BEFORE asking for data, never during the first incoming frame. Keep the
    # value equal to esptool.command's default so it will not change/restore it.
    from esptool.loader import DEFAULT_TIMEOUT
    esp._port.timeout = DEFAULT_TIMEOUT
    if not esp.IS_STUB:
        data = bytearray()
        for start in range(0, size, 0x10000):
            length = min(0x10000, size-start)
            part = esp.read_flash_slow(offset+start, length, None)
            remote = esp.flash_md5sum(offset+start, length)
            if len(part) != length or hashlib.md5(part).hexdigest() != remote.lower():
                raise RuntimeError(f'ROM read block length/MD5 mismatch at0x{offset+start:x}')
            data.extend(part)
            if size >= 0x100000 and len(data) % 0x40000 == 0:
                print(f'ROM backup/readback {len(data)}/{size}', flush=True)
        return bytes(data)
    if not 1 <= block_size <= 4096 or size <= 0:
        raise ValueError('Bounded reads require valid block/size')
    esp.check_command('bounded read flash', esp.ESP_CMDS['READ_FLASH'],
                      struct.pack('<IIII', offset, size, block_size, 1))
    data = bytearray()
    while len(data) < size:
        packet = esp.read()
        expected = min(block_size, size-len(data))
        if len(packet) != expected:
            raise RuntimeError(f'Bounded read short/long frame at0x{offset+len(data):x}: expected{expected}, got{len(packet)}')
        data.extend(packet)
        esp.write(struct.pack('<I', len(data)))
        if size >= 0x100000 and len(data) % 0x40000 == 0:
            print(f'Backup/readback {len(data)}/{size}', flush=True)
    remote = esp.read()
    if remote != hashlib.md5(data).digest():
        raise RuntimeError('Bounded read remote MD5 mismatch')
    return bytes(data)


def ota_entry(seq, state):
    crc = binascii.crc32(struct.pack('<I', seq), 0xffffffff) & 0xffffffff
    return struct.pack('<I20sII', seq, b'\xff'*20, state, crc)


def bootstrap_otadata():
    # One-time UART-trusted baseline, not an OTA health-acceptance result.
    # Avoid an unattended 90s candidate deadline while the user switches UART
    # back to P4 and installs its host firmware. All later SDIO updates use NEW.
    result = bytearray(b'\xff'*0x2000)
    result[:32] = ota_entry(1, 2)  # original factory ota_0, VALID fallback
    result[0x1000:0x1020] = ota_entry(2, 2)  # provisioned ota_1, trusted baseline
    return bytes(result)


def selected_slot(data):
    entries = []
    for off in (0, 0x1000):
        seq, _, state, crc = struct.unpack_from('<I20sII', data, off)
        if seq != 0xffffffff and crc == (binascii.crc32(struct.pack('<I', seq), 0xffffffff)&0xffffffff) and state not in (3, 4):
            entries.append((seq, state))
    return (max(entries)[0]-1) % 2 if entries else 0


def partition_entries(table):
    entries = {}
    for offset in range(0, len(table), 32):
        magic = table[offset:offset+2]
        if magic in (b'\xeb\xeb', b'\xff\xff'):
            break
        if magic != b'\xaa\x50':
            raise ValueError('Invalid partition table entry')
        _, kind, subtype, address, size, label, flags = struct.unpack_from('<HBBII16sI', table, offset)
        name = label.split(b'\0', 1)[0].decode('ascii')
        if name in entries or (flags and name in ('otadata', 'ota_0', 'ota_1')):
            raise ValueError('Duplicate or encrypted application/otadata partition')
        entries[name] = (kind, subtype, address, size)
    return entries


def validate_backup(backup, table):
    if len(backup) != SIZE or backup[0xc000:0xc000+len(table)] != table:
        raise ValueError('Unexpected C5 flash size/partition layout; no writes')
    entries = partition_entries(table)
    for name, expected in {'otadata': (1, 0, 0x16000, 0x2000),
                           'ota_0': (0, 0x10, 0x20000, 0x1e0000),
                           'ota_1': (0, 0x11, 0x200000, 0x1e0000)}.items():
        if entries.get(name) != expected:
            raise ValueError(f'Unexpected partition {name}')
    if selected_slot(backup[0x16000:0x18000]) != 0:
        raise ValueError('Initial provisioning requires original app selected in ota_0; do not overwrite active ota_1')
    from esptool.bin_image import ESP32C5FirmwareImage
    original = backup[0x20000:0x200000]
    parsed = ESP32C5FirmwareImage(io.BytesIO(original))
    if not parsed.append_digest:
        raise ValueError('Original fallback app has no SHA digest')
    return image_info(original[:parsed.data_length+32])


def allowed_writes(app, boot, otadata):
    writes = [(0x200000, app), (0x2000, boot), (0x16000, otadata)]
    if not 288 <= len(app) <= 0x1e0000 or not 32 <= len(boot) <= 0xa000 or len(otadata) != 0x2000:
        raise ValueError('Provision write range outside allowlist')
    return writes


def verify_preserved(before, after, writes):
    if len(before) != SIZE or len(after) != SIZE:
        raise ValueError('Full Flash readback missing')
    expected = bytearray(before)
    for offset, content in writes:
        # write_flash erases complete sectors, including final sector tail.
        end = (offset+len(content)+4095)//4096*4096
        expected[offset:end] = b'\xff'*(end-offset)
        expected[offset:offset+len(content)] = content
    if after != expected:
        raise RuntimeError('Flash changed outside allowed ranges or write/readback mismatch')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--uart-selected', required=True, choices=('c5',))
    p.add_argument('--baud', type=int, choices=(115200, 230400, 460800, 921600), default=460800,
                   help='ROM identifies at115200; RAM stub then switches to this rate before backup')
    p.add_argument('--no-stub', action='store_true', help='use ROM64-byte reads with remote MD5 per64KiB; no RAM stub')
    p.add_argument('--stub-version', choices=('1', '2'), default='2', help='select an official bundled RAM stub, without altering installed package files')
    p.add_argument('--build-dir', type=Path, default=BUILD)
    p.add_argument('--provision', action='store_true', help='explicit one-time bootloader/app/otadata write after backup')
    p.add_argument('--output', type=Path, default=meoave.ROOT/'firmware/p4_gesture/test-results'/('c5_uart_'+datetime.now().strftime('%Y%m%d_%H%M%S')))
    a = p.parse_args(argv)
    report = dict(ok=False, provision_requested=a.provision, flash_writes=[],
                  port=a.port, baud=a.baud,
                  scope='one-time UART trusted baseline; NOT SDIO OTA/rollback acceptance')
    esp = None
    created = False
    try:
        import esptool
        a.output.mkdir(parents=True, exist_ok=False)
        created = True
        app = (a.build_dir/'network_adapter.bin').read_bytes()
        boot = (a.build_dir/'bootloader/bootloader.bin').read_bytes()
        table = (a.build_dir/'partition_table/partition-table.bin').read_bytes()
        meta = image_info(app)
        if (b'FAULT_TEST pending image resets before Hosted' in app or
                b'HANG_TEST pending image blocks before Hosted' in app):
            raise ValueError('Never bootstrap a deliberate-fault app')
        header = (a.build_dir/'bootloader/config/sdkconfig.h').read_text(encoding='utf-8')
        if '#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' not in header:
            raise ValueError('Build bootloader is not rollback enabled')
        from esptool.bin_image import ESP32C5FirmwareImage
        parsed = ESP32C5FirmwareImage(io.BytesIO(boot))
        if parsed.chip_id != 23 or parsed.checksum != parsed.calculate_checksum() or not parsed.append_digest or parsed.stored_digest != parsed.calc_digest:
            raise ValueError('Invalid C5 bootloader image')
        writes = allowed_writes(app, boot, bootstrap_otadata())
        esp = esptool.detect_chip(a.port, baud=115200, connect_attempts=2)
        if esp.CHIP_NAME != 'ESP32-C5':
            raise RuntimeError(f'UART points to {esp.CHIP_NAME}, NOT C5; no Flash writes')
        security = esp.get_security_info()
        report['security'] = security
        if security['chip_id'] != 23 or security['flags'] & 5 or bin(security['flash_crypt_cnt']).count('1') % 2:
            raise RuntimeError('Secure boot/download/encryption or wrong chip; provisioning refused')
        report['revision'] = esp.get_chip_revision()
        if not meta['min_revision'] <= report['revision'] <= meta['max_revision']:
            raise RuntimeError('App does not support actual C5 revision')
        # RAM stub only. No eFuse, erase or flash write before persisted backup.
        if not a.no_stub:
            from esptool.loader import StubFlasher
            StubFlasher.set_stub_subdir(a.stub_version)
            report['stub_version_requested'] = a.stub_version
            esp = esp.run_stub()
        if a.baud != 115200:
            esp.change_baud(a.baud)
        esptool.cmds.attach_flash(esp)
        if esptool.cmds.detect_flash_size(esp) != '4MB':
            raise RuntimeError('Expected 4MB C5 Flash')
        esp.flash_set_parameters(SIZE)
        report['read_flow_control'] = dict(path='ROM' if a.no_stub else 'stub',
            block_bytes=64 if a.no_stub else 1024, max_inflight=1, remote_md5=True)
        before = read_bounded(esp, 0, SIZE)
        backup_path = a.output/'original_flash.bin'
        with backup_path.open('xb') as f:
            f.write(before); f.flush(); os.fsync(f.fileno())
        if backup_path.read_bytes() != before:
            raise RuntimeError('Backup disk readback failed')
        report['backup'] = dict(path=str(backup_path.resolve()), size=len(before), sha256=hashlib.sha256(before).hexdigest())
        report['original_app'] = validate_backup(before, table)
        report['new_app'] = meta
        if a.provision:
            for offset, content in writes:
                record = dict(offset=offset, size=len(content), sha256=hashlib.sha256(content).hexdigest(), verified=False)
                report['flash_writes'].append(record)
                esptool.cmds.write_flash(esp, [(offset, content)], flash_size='4MB',
                                        flash_mode='keep', flash_freq='keep')
                readback = read_bounded(esp, offset, len(content))
                if readback != content:
                    raise RuntimeError('Provision region readback mismatch; no next write')
                record['verified'] = True
            after = read_bounded(esp, 0, SIZE)
            verify_preserved(before, after, writes)
            (a.output/'provisioned_flash.bin').write_bytes(after)
            report['preserved_all_other_flash'] = True
            report['bootloader_sha256'] = hashlib.sha256(boot).hexdigest()
            report['runtime_health_verified'] = False
        esp.hard_reset()
        report['ok'] = True
        return 0
    except Exception as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print(report['error'], flush=True)
        return 1
    finally:
        if esp: esp._port.close()
        # Never overwrite another backup/report if the output directory existed.
        if created:
            (a.output/'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
