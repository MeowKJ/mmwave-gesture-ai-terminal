"""Offline safety/acceptance guards. Real Wi-Fi results are separate reports."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
import c5_wifi as w


class WifiTests(unittest.TestCase):
    def test_credentials_never_sent_under_persistent_or_insecure_build(self):
        safe=dict(MBEDTLS_CERTIFICATE_BUNDLE=True,MBEDTLS_HAVE_TIME_DATE=True,WIFI_RMT_NVS_ENABLED=False)
        w.validate_config(safe)
        for key,value in (('MBEDTLS_CERTIFICATE_BUNDLE',False),('MBEDTLS_HAVE_TIME_DATE',False),
                          ('WIFI_RMT_NVS_ENABLED',True),('ESP_WIFI_NVS_ENABLED',True)):
            with self.subTest(key=key),self.assertRaises(RuntimeError):w.validate_config(dict(safe,**{key:value}))

    def test_credentials_encoded_for_wire_not_plain_uart(self):
        ssid,password=w.credentials('Test Net','Unit-Only!')
        self.assertEqual(bytes.fromhex(ssid),b'Test Net')
        self.assertEqual(bytes.fromhex(password),b'Unit-Only!')

    def test_credentials_reject_length_nul_and_byte_overflow(self):
        for ssid,pw in [('', '12345678'),('a'*33,'12345678'),('a','short'),('a','x'*64),('a\0b','12345678'),('a','1234567\0'),('界'*11,'12345678')]:
            with self.subTest(ssid=ssid,pw=pw),self.assertRaises(ValueError): w.credentials(ssid,pw)
        w.credentials('a'*32,'b'*63)

    def test_redaction_covers_plain_hex_uppercase_nested_and_fault_tail(self):
        password='Unit-Only!'
        data={'uart_lines':[password,password.encode().hex(),password.encode().hex().upper()],
              'p4_fault_tail':'oops '+password,'error':{'more':password}}
        redacted=w.redact(data,password)
        text=str(redacted)
        self.assertNotIn(password,text)
        self.assertNotIn(password.encode().hex(),text)
        self.assertEqual(redacted['uart_lines'],['[REDACTED]']*3)

    def test_empty_redaction_does_not_destroy_output(self):
        self.assertEqual(w.redact({'ok':True,'a':'hello'},''),{'ok':True,'a':'hello'})

    def test_missing_or_duplicate_live_record_rejected(self):
        for lines in ([],['MEO:NET_IP ip=1','MEO:NET_IP ip=2']):
            with self.assertRaises(RuntimeError): w.record(lines,'MEO:NET_IP')

    class Link:
        def __init__(self,lines,error=None): self.lines,self.error=lines,error
        def command(self,*args,**kwargs):
            if self.error: raise self.error
            return self.lines,{}

    def probe_lines(self,**overrides):
        values=dict(error='0x0',status='200',body_bytes='123',complete='1',cert_verified='1')
        values.update(overrides)
        return ['MEO:NET_DNS target=example error=0 address=93.184.1.1 source=P4_LWIP',
                'MEO:NET_HTTPS target=example '+' '.join(f'{k}={v}' for k,v in values.items())]

    def test_verified_complete_real_probe_passes(self):
        self.assertTrue(w.probe_result(self.Link(self.probe_lines()),'example')['ok'])

    def test_status_incomplete_empty_unverified_or_error_do_not_pass(self):
        for wrong in (dict(status='302'),dict(status='404'),dict(body_bytes='0'),dict(complete='0'),
                      dict(cert_verified='0'),dict(error='0x107')):
            with self.subTest(wrong=wrong):
                self.assertFalse(w.probe_result(self.Link(self.probe_lines(**wrong)),'example')['ok'])

    def test_dns_target_mismatch_or_missing_https_does_not_pass(self):
        for lines in (self.probe_lines()[:1],self.probe_lines()+self.probe_lines(),
                      [s.replace('target=example','target=espressif') for s in self.probe_lines()],
                      [self.probe_lines()[0].replace('error=0','error=-1'),self.probe_lines()[1]]):
            self.assertFalse(w.probe_result(self.Link(lines),'example')['ok'])

    def test_explicit_probe_failure_is_recorded_not_retried(self):
        error=w.maintenance.MaintenanceCommandError('NET_PROBE',dict(error='0x107'),['terminal'])
        result=w.probe_result(self.Link([],error),'example')
        self.assertFalse(result['ok'])
        self.assertTrue(result['error'])

    def test_uart_timeout_and_p4_panic_never_become_endpoint_results(self):
        for error in (TimeoutError('UART timeout'),RuntimeError('P4 panic')):
            with self.assertRaises(type(error)): w.probe_result(self.Link([],error),'example')

    def test_final_c5_must_keep_same_identity_boot_valid_slot_and_highz(self):
        before=dict(elf_sha='a'*64,boot='12345678',slot=0x20000,boot_slot=0x20000,ota_state=2,pin_mode=0,lease_ms=0)
        w.same_c5(before,before.copy())
        for key,value in (('elf_sha','b'*64),('boot','87654321'),('slot',0x200000),('boot_slot',0x200000),
                          ('ota_state',1),('pin_mode',1),('lease_ms',100)):
            with self.subTest(key=key),self.assertRaises(RuntimeError): w.same_c5(before,dict(before,**{key:value}))

    def test_actual_c_decode_bounds(self):
        cc=os.environ.get('MEOAVE_HOST_CC') or shutil.which('gcc') or shutil.which('clang')
        if not cc:self.skipTest('native compiler required')
        source=(ROOT/'firmware/p4_gesture/bringup/main/wifi_diagnostic.c').read_text(encoding='utf-8')
        decode=source[source.index('static esp_err_t decode'):source.index('static esp_err_t scan')]
        code='''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
'''+decode+'''
int main(void) {
    uint8_t out[8]={0};
    assert(decode("4142",out,1,2)==0 && out[0]=='A' && out[1]=='B');
    assert(decode("4142",out,1,1)!=0);
    assert(decode("41",out,2,3)!=0);
    assert(decode("4100",out,1,2)!=0);
    assert(decode("4x",out,1,2)!=0);
    assert(decode("4",out,1,2)!=0);
    assert(decode(NULL,out,1,2)!=0);
    assert(decode("",out,1,2)!=0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'decode.c';path.write_text(code,encoding='utf-8')
            exe=path.with_suffix('.exe' if os.name=='nt' else '')
            result=subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(subprocess.run([str(exe)],timeout=5).returncode,0)

    def test_firmware_guardrails(self):
        source=(ROOT/'firmware/p4_gesture/bringup/main/wifi_diagnostic.c').read_text(encoding='utf-8')
        self.assertIn('cfg.nvs_enable=0;',source)
        self.assertIn('esp_wifi_set_storage(WIFI_STORAGE_RAM)',source)
        self.assertIn('.crt_bundle_attach=esp_crt_bundle_attach',source)
        self.assertIn('esp_http_client_is_complete_data_received',source)
        self.assertLess(source.index('esp_wifi_start()'),source.index('esp_wifi_set_band_mode('))
        self.assertNotIn('skip_cert_common_name_check',source)
        self.assertNotIn('esp_wifi_restore(',source)
        self.assertNotIn('gpio_set_level(',source)
        self.assertNotIn('nvs_flash_erase(',source)


if __name__=='__main__':unittest.main()
