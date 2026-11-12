"""Native USB framing/guards; these tests do not assert hardware acceptance."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
import c5_native as n


class Stream:
    def __init__(self,body=None,end_id=None,tx='1'):
        self.body=body or ['MEO:MAINT_REPLY op=ATTACH token=none error=0x0 state=READY offset=0 total=0']
        self.end_id,self.tx=end_id,tx
        self.writes=[];self.response=b''
    @property
    def in_waiting(self): return len(self.response)
    def write(self,data):
        self.writes.append(data)
        rid=data.decode().split()[1]
        self.response=('MEO:NATIVE_BEGIN id='+rid+'\n'+'\n'.join(self.body)+'\n'+
                       f'MEO:NATIVE_END id={self.end_id or rid} tx_ok={self.tx} init_error=0x0\n').encode()
        return len(data)
    def flush(self): pass
    def read(self,count):
        out,self.response=self.response[:count],self.response[count:]
        return out


class NativeTests(unittest.TestCase):
    def test_matching_frame_has_exactly_one_write(self):
        stream=Stream();link=n.NativeLink(stream,'',{})
        _,reply=link.command('ATTACH')
        self.assertEqual(reply['state'],'READY')
        self.assertEqual(len(stream.writes),1)
        self.assertIn(b' ATTACH\n',stream.writes[0])

    def test_reject_stale_end_and_partial_output(self):
        for stream in (Stream(end_id='stale'),Stream(tx='0')):
            with self.subTest(stream=stream),self.assertRaises(RuntimeError):
                n.NativeLink(stream,'',{}).command('ATTACH')
            self.assertEqual(len(stream.writes),1)

    def test_device_error_preserves_explicit_reply_no_retry(self):
        stream=Stream(['MEO:MAINT_REPLY op=ATTACH token=none error=0x107 state=FAILED offset=0 total=0'])
        with self.assertRaises(n.m.MaintenanceCommandError) as caught:
            n.NativeLink(stream,'',{}).command('ATTACH')
        self.assertEqual(caught.exception.error,0x107)
        self.assertEqual(len(stream.writes),1)

    def test_duplicate_reply_not_success(self):
        stream=Stream();stream.body*=2
        with self.assertRaises(RuntimeError):n.NativeLink(stream,'',{}).command('ATTACH')

    def test_wrong_ota_token_offset_or_op_rejected(self):
        for body in ['op=OTA_WRITE token=other error=0x0 offset=512',
                     'op=OTA_WRITE token=right error=0x0 offset=1',
                     'op=OTA_END token=right error=0x0 offset=512']:
            stream=Stream(['MEO:MAINT_REPLY '+body])
            with self.subTest(body=body),self.assertRaises(RuntimeError):
                n.NativeLink(stream,'',{}).command('OTA_WRITE right 0 aa',token='right',offset=512)

    def test_full_512_byte_write_fits_native_frame(self):
        stream=Stream();n.NativeLink(stream,'',{}).transaction('OTA_WRITE abc 0 '+'ab'*512)
        self.assertLess(len(stream.writes[0]),1400)
        self.assertGreater(len(stream.writes[0]),1024)

    def test_injection_or_overflow_never_sent(self):
        for command in ('ATTACH\nCONNECT','ATTACH\rDOWNLOAD','x'*1400,'界',''):
            stream=Stream()
            with self.subTest(command=command[:30]),self.assertRaises(ValueError):
                n.NativeLink(stream,'',{}).transaction(command)
            self.assertFalse(stream.writes)

    def test_native_identity_is_exact_product_not_headless_or_stale(self):
        identity='MEO:BRINGUP project=p4_gesture profile=maintenance elf_sha='+'a'*64
        status='MEO:MAINT_STATUS state=IDLE'
        self.assertEqual(n.NativeLink(Stream([identity,status]),'a'*64,{}).identify()['state'],'IDLE')
        for bad in (identity.replace('p4_gesture','p4_c5_bringup'),identity.replace('maintenance','hosted'),identity.replace('a'*64,'b'*64)):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):
                n.NativeLink(Stream([bad,status]),'a'*64,{}).identify()

    def test_required_product_configuration(self):
        safe={k:True for k in ('APP_C5_NATIVE_MAINTENANCE','APP_MEOAVE_UI',
                              'APP_FLS2_OFFICIAL_BASELINE','APP_USB_HID_ENABLE','ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER')}
        n.validate_config(safe)
        for k in safe:
            with self.subTest(key=k),self.assertRaises(RuntimeError):n.validate_config(dict(safe,**{k:False}))
        for k in ('APP_C5_SDIO_LOADER','APP_AUDIO_BEEP'):
            with self.assertRaises(RuntimeError):n.validate_config(dict(safe,**{k:True}))

    def test_actual_c_usb_parser_discards_overflow_tail(self):
        cc=os.environ.get('MEOAVE_HOST_CC') or shutil.which('gcc')
        if not cc:self.skipTest('native GCC unavailable')
        source=(ROOT/'firmware/p4_gesture/main/bsp_usb.c').read_text(encoding='utf-8')
        actual=source[source.index('static void cdc_rx_cb('):source.index('/* 兼容 esptool',source.index('static void cdc_rx_cb('))]
        harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define CDC_LINE_MAX 1400
#define ESP_OK 0
#define TINYUSB_CDC_ACM_0 0
typedef void cdcacm_event_t;
static char s_line[CDC_LINE_MAX];static size_t s_line_len;
static bool s_line_overflow,s_rebooting;
static const char *input;static size_t remaining;
static unsigned dispatched,downloads;
static void dispatch_line(const char *s){dispatched++;if(!strcmp(s,"DOWNLOAD"))downloads++;}
static int tinyusb_cdcacm_read(int port,uint8_t *buf,size_t max,size_t *n){
 (void)port;*n=remaining<max?remaining:max;memcpy(buf,input,*n);input+=*n;remaining-=*n;return ESP_OK;
}
'''+actual+r'''
static void feed(const char *p,size_t n){input=p;remaining=n;cdc_rx_cb(0,NULL);}
int main(void){
 char huge[1500];memset(huge,'a',sizeof(huge));
 memcpy(huge+1400,"DOWNLOAD\n",9);feed(huge,1409);
 assert(!dispatched && !downloads);
 feed("PING\n",5);assert(dispatched==1);
 char ota[1150];memset(ota,'a',sizeof(ota));ota[sizeof(ota)-1]='\n';
 feed(ota,500);assert(dispatched==1);feed(ota+500,sizeof(ota)-500);assert(dispatched==2);
 feed("DOWNLOAD\n",9);assert(downloads==1 && dispatched==3);
 for(unsigned i=0;i<sizeof(s_line);i++)assert(!s_line[i]);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'native.c';exe=Path(d)/'native.exe';c.write_text(harness,encoding='utf-8')
            subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,capture_output=True)


if __name__=='__main__':unittest.main()
