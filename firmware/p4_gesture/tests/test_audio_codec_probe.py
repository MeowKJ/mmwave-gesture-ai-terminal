"""Compile the actual codec identity/readback gates against faulty I2C doubles."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


class CodecProbeRegression(unittest.TestCase):
    def test_identity_and_config_gates(self):
        compiler = shutil.which(os.environ.get("CC", "gcc"))
        self.assertIsNotNone(compiler, "GCC is required for codec gate regression")
        source = (Path(__file__).resolve().parents[1] / "main/bsp_audio.c").read_text(encoding="utf-8")
        begin = source.index("static esp_err_t codec_identify(")
        gates = source[begin:source.index("/* ---------------- shared numeric helpers", begin)]
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_ERR_NO_MEM 3
#define I2C_ADDR_BIT_LEN_7 0
#define CONFIG_APP_AUDIO_CODEC_INIT 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef void *i2c_master_bus_handle_t;
typedef struct { int address; } device;
typedef device *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
static device dev;
static bool ack[128], add_failure;
static uint8_t chips[128][2], registers[256], s_chip_id[2], s_addr;
static const uint8_t s_addr_candidates[] = {0x12,0x10,0x11,0x13};
static bool s_present;
static uint32_t s_i2c_acks;
static i2c_master_dev_handle_t s_codec;
static int removals, reads, read_failure, unstable_at;
static esp_err_t i2c_master_probe(void *bus, uint8_t addr, int timeout)
{ (void)bus;(void)timeout;return ack[addr] ? ESP_OK : ESP_FAIL; }
static esp_err_t i2c_master_bus_add_device(void *bus,const i2c_device_config_t *c,device **out)
{ (void)bus;if(add_failure)return ESP_ERR_NO_MEM;dev.address=c->device_address;*out=&dev;return ESP_OK; }
static esp_err_t i2c_master_bus_rm_device(device *d)
{ assert(d==&dev);++removals;return ESP_OK; }
static esp_err_t i2c_master_transmit_receive(device *d,const uint8_t *reg,size_t n,uint8_t *v,size_t m,int timeout)
{
 (void)timeout;assert(d==&dev && n==1 && m==1);++reads;
 if(reads==read_failure)return ESP_FAIL;
 *v=(*reg==0xFD || *reg==0xFE) ? chips[dev.address][*reg-0xFD] : registers[*reg];
 if(reads==unstable_at)*v=0xFF;
 return ESP_OK;
}
'''
        tests = r'''
static void clear(void) {
 memset(ack,0,sizeof(ack));memset(chips,0xFF,sizeof(chips));
 memset(registers,0,sizeof(registers));s_codec=NULL;s_present=false;
 removals=reads=read_failure=unstable_at=0;add_failure=false;
}
int main(void) {
 clear();ack[0x12]=true;chips[0x12][0]=0x83;chips[0x12][1]=0x89;
 assert(codec_identify(NULL)==ESP_OK && s_present && s_addr==0x12 && reads==4);
 assert(s_chip_id[0]==0x83 && s_chip_id[1]==0x89);
 clear();ack[0x12]=ack[0x10]=true;chips[0x10][0]=0x83;chips[0x10][1]=0x89;
 assert(codec_identify(NULL)==ESP_OK && s_addr==0x10 && removals==1);
 clear();ack[0x10]=true;
 assert(codec_identify(NULL)==ESP_ERR_NOT_FOUND && !s_present && s_codec==NULL);
 clear();ack[0x12]=true;chips[0x12][0]=0x83;chips[0x12][1]=0x89;unstable_at=4;
 assert(codec_identify(NULL)==ESP_ERR_NOT_FOUND && !s_present && removals==1);
 clear();ack[0x12]=true;read_failure=1;
 assert(codec_identify(NULL)==ESP_ERR_NOT_FOUND && !s_present);
 clear();ack[0x12]=true;add_failure=true;
 assert(codec_identify(NULL)==ESP_ERR_NO_MEM && !s_present);
 clear();ack[0x12]=true;chips[0x12][0]=0x83;chips[0x12][1]=0x89;
 assert(codec_identify(NULL)==ESP_OK);
 registers[1]=0x08;registers[0x20]=registers[0x40]=0x60;
 registers[0x72]=0x64;registers[0x73]=0x54;
 assert(codec_verify_config()==ESP_OK);
 registers[0x40]=0xFF;assert(codec_verify_config()==ESP_ERR_INVALID_RESPONSE);
 registers[0x40]=0x60;read_failure=reads+1;assert(codec_verify_config()==ESP_FAIL);
 puts("PASS: identity rejects phantom ACK/FF/NACK/unstable ID; register readback rejects partial init");
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="meoave-codec-") as tmp:
            root = Path(tmp)
            cfile = root / "codec_gate.c"
            binary = root / ("codec_gate.exe" if os.name == "nt" else "codec_gate")
            cfile.write_text(prelude + gates + tests, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
