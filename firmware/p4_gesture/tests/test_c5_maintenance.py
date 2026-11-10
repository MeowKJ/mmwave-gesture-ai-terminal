"""Offline host/provision guards. These tests are NOT hardware acceptance."""
import binascii
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools'))
import c5_maintenance as m
import c5_provision as p


def app_image(chip=23):
    header = bytearray(24)
    header[0:2] = b'\xe9\x01'
    header[12:14] = struct.pack('<H', chip)
    struct.pack_into('<HH', header, 15, 100, 199)
    header[23] = 1
    app = bytearray(256)
    app[:4] = b'\x32\x54\xcd\xab'
    app[144:176] = bytes(range(32))
    data = header + struct.pack('<II', 0x42000020, len(app)) + app
    checksum = 0xef
    for value in app: checksum ^= value
    data += b'\0'*((15-len(data))%16) + bytes([checksum])
    return bytes(data) + hashlib.sha256(data).digest()


class Stream:
    def __init__(self, response=b''):
        self.response, self.writes = response, []
    def read(self, n):
        data, self.response = self.response[:n], self.response[n:]
        return data
    def write(self, data): self.writes.append(data); return len(data)
    def flush(self): pass


def record(**kw):
    r = dict(elf_sha='1'*64, boot='12345678', slot=0x20000, boot_slot=0x20000,
             ota_state=2, flags=11, pin_mode=0, uptime_ms=5000)
    r.update(kw)
    return r


class FakeLink:
    def __init__(self, snapshots):
        self.snapshots, self.commands, self.report = list(snapshots), [], {}
    def inspect(self): return self.snapshots.pop(0)
    def command(self, command, **kwargs): self.commands.append(command); return [], {}
    def reset_p4(self): self.commands.append('P4RESET')
    def confirm(self, elf, slot): self.commands.append('CONFIRM '+elf); return record(elf_sha=elf, slot=slot)


class ImageTests(unittest.TestCase):
    def test_failure_candidates_cannot_be_normal_updates(self):
        for marker in (m.FAULT_MARKER, m.HANG_MARKER):
            for mode in ('update', 'unconfirmed'):
                with self.subTest(marker=marker, mode=mode), self.assertRaises(ValueError):
                    m.validate_candidate_mode(app_image()+marker, mode)

    def test_failure_modes_require_their_own_marker(self):
        for mode, marker, wrong in (('fault', m.FAULT_MARKER, m.HANG_MARKER),
                                    ('hung', m.HANG_MARKER, m.FAULT_MARKER)):
            m.validate_candidate_mode(app_image()+marker, mode)
            for data in (app_image(), app_image()+wrong, app_image()+marker+wrong):
                with self.subTest(mode=mode, data=data[-20:]), self.assertRaises(ValueError):
                    m.validate_candidate_mode(data, mode)

    def test_valid_single_c5_app(self):
        data = app_image()
        info = m.image_info(data)
        self.assertEqual(info['elf_sha'], bytes(range(32)).hex())
        self.assertEqual(info['min_revision'], 100)
        self.assertEqual(info['max_revision'], 199)
        self.assertEqual(info['sha256'], hashlib.sha256(data).hexdigest())

    def test_wrong_chip(self):
        with self.assertRaises(ValueError): m.image_info(app_image(18))

    def test_merged(self):
        with self.assertRaises(ValueError): m.image_info(b'\xff'*8192+app_image())

    def test_trailing_data(self):
        with self.assertRaises(ValueError): m.image_info(app_image()+b'\xff'*32)

    def test_corrupt_digest(self):
        data=bytearray(app_image()); data[-1] ^= 1
        with self.assertRaises(ValueError): m.image_info(data)

    def test_corrupt_descriptor(self):
        data=bytearray(app_image()); data[32] ^= 1
        with self.assertRaises(ValueError): m.image_info(data)


class UartTests(unittest.TestCase):
    def test_explicit_error_is_not_uart_timeout(self):
        stream = Stream(b'MEO:MAINT_REPLY op=ATTACH token=none error=0x107 state=FAILED offset=0 total=0\n')
        with self.assertRaises(m.MaintenanceCommandError) as caught:
            m.Link(stream, '', {}).command('ATTACH')
        self.assertEqual(caught.exception.error, 0x107)
        self.assertEqual(caught.exception.op, 'ATTACH')
        self.assertEqual(len(stream.writes), 1)

    def test_short_ack_read_does_not_request_4k(self):
        class SerialStream(Stream):
            @property
            def in_waiting(self): return len(self.response)
            def read(self, n):
                self.requested = n
                return super().read(n)
        stream = SerialStream(b'ACK\n')
        self.assertEqual(m.Link(stream, '', {}).line(m.time.monotonic()+1), 'ACK')
        self.assertEqual(stream.requested, 4)

    def test_matching_command(self):
        stream=Stream(b'MEO:MAINT_REPLY op=OTA_WRITE token=abcd error=0x0 state=WRITING offset=512 total=1024\n')
        _, r=m.Link(stream, '0'*64, {}).command('OTA_WRITE abcd 0 00', token='abcd', offset=512)
        self.assertEqual(r['offset'], '512')
        self.assertEqual(len(stream.writes), 1)

    def test_lost_reply_not_retried(self):
        stream=Stream()
        with self.assertRaises(TimeoutError):
            m.Link(stream, '0'*64, {}).command('OTA_ACTIVATE abcd', timeout=0)
        self.assertEqual(stream.writes, [b'OTA_ACTIVATE abcd\n'])

    def test_stale_token_rejected(self):
        stream=Stream(b'MEO:MAINT_REPLY op=OTA_WRITE token=old error=0x0 offset=512\n')
        with self.assertRaises(RuntimeError):
            m.Link(stream, '0'*64, {}).command('OTA_WRITE new 0 00', token='new', offset=512)
        self.assertEqual(len(stream.writes), 1)

    def test_wrong_offset_rejected(self):
        stream=Stream(b'MEO:MAINT_REPLY op=OTA_WRITE token=abcd error=0x0 offset=1024\n')
        with self.assertRaises(RuntimeError):
            m.Link(stream, '0'*64, {}).command('OTA_WRITE abcd 0 00', token='abcd', offset=512)

    def test_duplicate_fields_rejected(self):
        with self.assertRaises(ValueError): m.fields('X error=0 error=1', 'X')

    def test_p4_panic_stops(self):
        with self.assertRaises(RuntimeError): m.Link(Stream(b'Guru Meditation Error\n'), '', {}).line(m.time.monotonic()+1)

    def test_wrong_p4_profile_rejected(self):
        stream=Stream(b'MEO:BRINGUP project=p4_c5_bringup profile=hosted elf_sha='+b'a'*64+b'\nMEO:MAINT_STATUS state=IDLE\n')
        with self.assertRaises(RuntimeError): m.Link(stream, 'a'*64, {}).identify()
        self.assertEqual(stream.writes, [b'INFO\n'])

    def test_missing_c5_live_response(self):
        with self.assertRaises(RuntimeError): m.c5_record([])


class TransactionTests(unittest.TestCase):
    def run_update(self, mode, snapshots):
        data=app_image(); meta=m.image_info(data); link=FakeLink(snapshots)
        with patch.object(m.time, 'sleep'):
            m.update(link, data, meta, mode)
        return link

    def test_normal_update_no_en_reset_on_pending(self):
        meta=m.image_info(app_image())
        link=self.run_update('update', [record(), record(elf_sha=meta['elf_sha'], boot='22345678', slot=0x200000, boot_slot=0x200000, ota_state=1)])
        self.assertIn('ATTACH', link.commands)
        self.assertNotIn('CONNECT', link.commands)
        self.assertEqual(sum(c.startswith('OTA_ACTIVATE ') for c in link.commands), 1)
        self.assertEqual(sum(c.startswith('CONFIRM ') for c in link.commands), 1)

    def test_interrupted_never_activates(self):
        link=self.run_update('interrupted', [record(), record(boot='22345678')])
        self.assertIn('CONNECT', link.commands)
        self.assertFalse(any(c.startswith(('OTA_END ', 'OTA_ACTIVATE ', 'CONFIRM ')) for c in link.commands))

    def test_fault_image_must_restore_old_slot(self):
        link=self.run_update('fault', [record(), record(boot='22345678')])
        self.assertTrue(link.report['transactions'][0]['bootloader_fault_rollback_verified'])
        self.assertFalse(any(c.startswith('CONFIRM ') for c in link.commands))

    def test_unconfirmed_timeout_is_not_confirmed(self):
        meta=m.image_info(app_image())
        link=self.run_update('unconfirmed', [record(), record(elf_sha=meta['elf_sha'], boot='22345678', slot=0x200000, boot_slot=0x200000, ota_state=1), record(boot='32345678')])
        self.assertTrue(link.report['transactions'][0]['timeout_rollback_verified'])
        self.assertFalse(any(c.startswith('CONFIRM ') for c in link.commands))

    def test_unconfirmed_baseline_cannot_erase(self):
        link=FakeLink([record(ota_state=1)])
        with self.assertRaises(RuntimeError): m.update(link, app_image(), m.image_info(app_image()))
        self.assertEqual(link.commands, [])

    def test_old_image_reappearance_is_not_update_success(self):
        with self.assertRaises(RuntimeError): self.run_update('update', [record(), record(boot='22345678')])


class HungRecoveryTests(unittest.TestCase):
    class HungLink(FakeLink):
        def __init__(self, snapshots, failure):
            super().__init__(snapshots)
            self.failure = failure
        def command(self, command, **kwargs):
            result = super().command(command, **kwargs)
            if command == 'ATTACH' and self.failure: raise self.failure
            return result

    def run_hung(self, failure, restored=None):
        link = self.HungLink([record(), restored or record(boot='22345678')], failure)
        with patch.object(m.time, 'sleep'):
            m.update(link, app_image(), m.image_info(app_image()), 'hung')
        return link

    def test_explicit_transport_timeout_then_single_en_rollback(self):
        failure = m.MaintenanceCommandError('ATTACH', dict(error='0x107', state='FAILED'), ['reply'])
        link = self.run_hung(failure)
        self.assertTrue(link.report['transactions'][0]['hung_candidate_en_rollback_verified'])
        self.assertTrue(link.report['automatic_recovery']['ok'])
        self.assertEqual(link.commands.count('CONNECT'), 1)
        self.assertEqual(link.commands.count('ATTACH'), 2)
        self.assertEqual(link.report['transactions'][0]['unresponsive_wait_seconds'], 95)
        self.assertFalse(any(c.startswith('CONFIRM ') for c in link.commands))

    def test_absent_card_with_explicit_bus_failure_can_recover(self):
        failure = m.MaintenanceCommandError('ATTACH', dict(error='0xffffffff', state='FAILED'),
                                           ['ensure_slave_bus_ready failed', 'reply'])
        self.assertTrue(self.run_hung(failure).report['automatic_recovery']['ok'])

    def test_unexpected_failures_never_pass_hung_test(self):
        failures = [TimeoutError('UART lost'), RuntimeError('P4 panic'),
                    m.MaintenanceCommandError('ATTACH', dict(error='0x103', state='FAILED'), ['reply']),
                    m.MaintenanceCommandError('ATTACH', dict(error='0xffffffff', state='FAILED'), ['reply']),
                    m.MaintenanceCommandError('VERIFY', dict(error='0x107', state='FAILED'), ['reply'])]
        for failure in failures:
            with self.subTest(failure=failure), self.assertRaises((RuntimeError, TimeoutError)):
                self.run_hung(failure)

    def test_successful_attach_means_injection_not_proven(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpectedly provided Hosted'):
            self.run_hung(None)

    def test_recovery_is_bounded_even_when_it_fails(self):
        link = FakeLink([record(ota_state=1)])
        transaction = dict(source=record())
        with patch.object(m.time, 'sleep'):
            with self.assertRaises(RuntimeError): m.recover_previous(link, transaction)
            with self.assertRaisesRegex(RuntimeError, 'already attempted'): m.recover_previous(link, transaction)
        self.assertEqual(link.commands.count('CONNECT'), 1)
        self.assertFalse(link.report['automatic_recovery']['ok'])

    def test_recovery_refuses_wrong_slot_or_pin_owned(self):
        for bad in (record(slot=0x200000), record(pin_mode=1), record(boot_slot=0x200000)):
            link = FakeLink([bad])
            with patch.object(m.time, 'sleep'), self.assertRaises(RuntimeError):
                m.recover_previous(link, dict(source=record()))
            self.assertFalse(link.report['automatic_recovery']['ok'])


class ResetSoakTests(unittest.TestCase):
    class SoakLink(FakeLink):
        def attach(self, reset_c5=False):
            if reset_c5: raise AssertionError('C5 must not be reset')
            return self.inspect()

    def test_same_c5_boot_survives_p4_resets(self):
        link = self.SoakLink([record(), record()])
        m.reset_soak(link, record(), 2)
        self.assertEqual(link.commands, ['P4RESET', 'P4RESET'])
        self.assertEqual(len(link.report['reset_cycles']), 2)

    def test_changed_boot_stops_before_next_reset(self):
        link = self.SoakLink([record(boot='22345678'), record()])
        with self.assertRaises(RuntimeError): m.reset_soak(link, record(), 2)
        self.assertEqual(link.commands, ['P4RESET'])
        self.assertNotIn('reset_cycles', link.report)

    def test_pending_baseline_refused(self):
        link = self.SoakLink([])
        with self.assertRaises(RuntimeError): m.reset_soak(link, record(ota_state=1), 2)
        self.assertEqual(link.commands, [])


class BaselineRecoveryTests(unittest.TestCase):
    class RecoveryLink(FakeLink):
        def identify(self): self.commands.append('INFO')

    def test_single_en_reset_before_inspecting_valid_baseline(self):
        link = self.RecoveryLink([record()])
        self.assertEqual(m.recover_baseline(link)['ota_state'], 2)
        self.assertEqual(link.commands, ['INFO', 'P4RESET', 'CONNECT'])
        self.assertTrue(link.report['baseline_recovery']['ok'])
        self.assertEqual(link.report['baseline_recovery']['max_c5_en_resets'], 1)

    def test_pending_or_shared_pin_owned_is_not_a_valid_baseline(self):
        for bad in (record(ota_state=1), record(pin_mode=1), record(boot_slot=0x200000)):
            link = self.RecoveryLink([bad])
            with self.assertRaises(RuntimeError): m.recover_baseline(link)
            self.assertFalse(link.report['baseline_recovery']['ok'])
            self.assertEqual(link.commands.count('CONNECT'), 1)

    def test_recovery_cannot_silently_mutate_read_only_actions(self):
        with self.assertRaises(SystemExit):
            m.main(['check', '--port', 'COM99', '--recover-baseline'])


class ProvisionTests(unittest.TestCase):
    def test_stub_bounded_read_ack_and_digest(self):
        class Stub:
            IS_STUB=True
            ESP_CMDS={'READ_FLASH':0xd2}
            _port=type('Port', (), {'timeout':1})()
            def __init__(self):
                self.parts=iter([b'a'*1024,b'b'*10,hashlib.md5(b'a'*1024+b'b'*10).digest()])
                self.acks=[]
            def check_command(self, _, opcode, args):
                self.request=(opcode,args)
                self.timeout_at_request=self._port.timeout
            def read(self): return next(self.parts)
            def write(self, data): self.acks.append(struct.unpack('<I',data)[0])
        stub=Stub()
        self.assertEqual(p.read_bounded(stub,0x1000,1034),b'a'*1024+b'b'*10)
        self.assertEqual(stub.request,(0xd2,struct.pack('<IIII',0x1000,1034,1024,1)))
        self.assertEqual(stub.acks,[1024,1034])
        self.assertEqual(stub.timeout_at_request,3)

    def test_stub_short_packet_stops_without_ack(self):
        class Stub:
            IS_STUB=True
            ESP_CMDS={'READ_FLASH':0xd2}
            _port=type('Port', (), {'timeout':1})()
            def check_command(self,*args): pass
            def read(self): return b'x'*956
            def write(self,data): raise AssertionError('short packet was acknowledged')
        with self.assertRaises(RuntimeError): p.read_bounded(Stub(),0,1024)

    def test_rom_read_checks_each_chunk_digest(self):
        class Rom:
            IS_STUB=False
            _port=type('Port', (), {'timeout':1})()
            def read_flash_slow(self,offset,length,progress): return bytes([offset//0x10000])*length
            def flash_md5sum(self,offset,length): return hashlib.md5(bytes([offset//0x10000])*length).hexdigest()
        self.assertEqual(p.read_bounded(Rom(),0,0x10001),b'\0'*0x10000+b'\1')

    def test_rom_digest_mismatch_stops(self):
        class Rom:
            IS_STUB=False
            _port=type('Port', (), {'timeout':1})()
            def read_flash_slow(self,offset,length,progress): return b'x'*length
            def flash_md5sum(self,offset,length): return '0'*32
        with self.assertRaises(RuntimeError): p.read_bounded(Rom(),0,64)

    def test_uart_trusted_bootstrap_entries(self):
        data=p.bootstrap_otadata()
        self.assertEqual(len(data), 8192)
        self.assertEqual(p.selected_slot(data), 1)
        for offset, seq in ((0,1), (4096,2)):
            got, label, state, crc=struct.unpack_from('<I20sII', data, offset)
            self.assertEqual((got,label,state), (seq,b'\xff'*20,2))
            self.assertEqual(crc, binascii.crc32(struct.pack('<I',seq),0xffffffff)&0xffffffff)

    def test_empty_otadata_uses_original_slot(self):
        self.assertEqual(p.selected_slot(b'\xff'*8192), 0)

    def test_write_scope(self):
        writes=p.allowed_writes(app_image(), b'\0'*100, p.bootstrap_otadata())
        self.assertEqual([o for o,_ in writes], [0x200000,0x2000,0x16000])

    def test_bootloader_must_not_erase_table(self):
        with self.assertRaises(ValueError): p.allowed_writes(app_image(), b'\0'*0xa001, p.bootstrap_otadata())

    def test_app_slot_capacity(self):
        with self.assertRaises(ValueError): p.allowed_writes(b'\0'*0x1e0001, b'\0'*100, p.bootstrap_otadata())

    def test_preservation_includes_erased_tail(self):
        before=b'\xa5'*p.SIZE
        writes=[(0x200000,b'123')]
        after=bytearray(before); after[0x200000:0x201000]=b'123'+b'\xff'*4093
        p.verify_preserved(before, bytes(after), writes)
        after[0x10000] ^= 1
        with self.assertRaises(RuntimeError): p.verify_preserved(before, bytes(after), writes)


class ServiceNativeTests(unittest.TestCase):
    def test_actual_c5_decision_functions(self):
        cc=os.environ.get('MEOAVE_HOST_CC') or shutil.which('gcc') or shutil.which('clang')
        if not cc: self.skipTest('native C compiler required; not hardware acceptance')
        source=(ROOT/'firmware/c5_hosted/main/c5_maintenance.c').read_text(encoding='utf-8')
        pin=source[source.index('static esp_err_t pin_apply'):source.index('static void respond')]
        decisions=source[source.index('static void advance_timers'):source.index('static void worker')]
        fixture=(Path(__file__).parent/'fixtures/c5_maintenance_harness.c').read_text(encoding='utf-8')
        code=fixture.replace('PROTOCOL_HEADER', (ROOT/'firmware/shared/c5_maintenance_protocol.h').as_posix()).replace('/* ACTUAL_PIN_APPLY */',pin).replace('/* ACTUAL_DECISIONS */',decisions)
        with tempfile.TemporaryDirectory(prefix='c5-maint-') as temp:
            path=Path(temp)/'maintenance.c'; path.write_text(code,encoding='utf-8')
            exe=path.with_suffix('.exe' if os.name=='nt' else '')
            compiled=subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            for case in ('no_auto_confirm','confirm','insufficient_health','stale_nonce','wrong_hash','too_early',
                         'pin_prevents_confirm','duplicate_health','lease_expires','lease_bounds',
                         'break_before_make','pull_failure','release_failure','unconfirmed_timeout',
                         'valid_no_timeout','stale_pin_request'):
                with self.subTest(case=case):
                    result=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__': unittest.main()
