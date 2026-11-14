"""Execute the complete product BSP with deterministic IDF/RTOS doubles.

Not a radio/SDIO hardware test. No serial device, reset or flash is used.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DOUBLES = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "bsp_wifi_hosted.h"
typedef int esp_err_t; typedef int portMUX_TYPE; typedef int nvs_handle_t;
typedef int esp_event_base_t; typedef int esp_event_handler_instance_t; typedef int esp_netif_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
enum {WIFI_EVENT_SCAN_DONE=1,WIFI_EVENT_STA_CONNECTED,WIFI_EVENT_STA_DISCONNECTED};
enum {IP_EVENT_STA_GOT_IP=1,IP_EVENT_STA_LOST_IP};
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ 40000
#define PIN_C5_RESET 54
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_PS_NONE 0
#define WIFI_BAND_MODE_AUTO 0
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_OPEN 0
#define WPA3_SAE_PWE_BOTH 0
#define WIFI_SCAN_TYPE_ACTIVE 0
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) 1u,2u,3u,4u
typedef struct {uint32_t status;} wifi_event_sta_scan_done_t;
typedef struct {int reason;} wifi_event_sta_disconnected_t;
typedef struct {uint8_t ssid[32],ssid_len;} wifi_event_sta_connected_t;
typedef struct {struct {uint32_t ip;} ip_info;} ip_event_got_ip_t;
typedef struct {uint8_t ssid[33];int rssi;} wifi_ap_record_t;
typedef struct {int nvs_enable;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){.nvs_enable=1})
typedef struct {struct {uint8_t ssid[32],password[64];struct {int authmode;} threshold;int sae_pwe_h2e;} sta;} wifi_config_t;
typedef struct {int scan_type;struct {struct {int min,max;} active;} scan_time;} wifi_scan_config_t;
static int64_t fake_time;static int creates,scans,stops,clears,record_gets,writes,commits,connects,config_calls;
static int scan_error,records_error,commit_error,disconnect_error,attach_calls;
static bool reset_enabled,stored_valid;static uint16_t scan_count;
static wifi_ap_record_t records[32];static wifi_config_t config;
static unsigned char stored[98];static char usb_line[1024];
static int64_t esp_timer_get_time(void){return fake_time;}
static int xTaskCreate(void (*fn)(void*),const char*n,int stack,void*a,int prio,void*h)
{(void)fn;(void)n;(void)stack;(void)a;(void)prio;(void)h;++creates;return pdPASS;}
static void vTaskDelete(void*p){(void)p;}
static void vTaskDelay(int t){(void)t;}
static const char *esp_err_to_name(int n){(void)n;return "fake";}
static int esp_hosted_sdio_diagnostic_configure_ex(unsigned w,unsigned k,unsigned p,unsigned t)
{assert(w==4&&k==40000&&p==0&&t==10000);return 0;}
static int esp_hosted_sdio_diagnostic_set_reset(bool b){reset_enabled=b;return 0;}
static int esp_hosted_connect_to_slave(void){++attach_calls;return 0;}
static int esp_hosted_get_cp_info(uint32_t*i,char*c,size_t n){*i=1;snprintf(c,n,"esp32c5");return 0;}
static int esp_netif_init(void){return 0;}
static int esp_event_loop_create_default(void){return 0;}
static esp_netif_t *esp_netif_create_default_wifi_sta(void){static int n;return &n;}
static int esp_event_handler_instance_register(int b,int id,void(*f)(void*,int,int32_t,void*),void*a,int*h)
{(void)b;(void)id;(void)f;(void)a;if(h)*h=1;return 0;}
static int esp_event_handler_instance_unregister(int b,int id,int h){(void)b;(void)id;(void)h;return 0;}
static int esp_wifi_init(wifi_init_config_t*c){assert(!c->nvs_enable);return 0;}
static int esp_wifi_set_storage(int x){(void)x;return 0;}
static int esp_wifi_set_mode(int x){(void)x;return 0;}
static int esp_wifi_set_ps(int x){(void)x;return 0;}
static int esp_wifi_start(void){return 0;}
static int esp_wifi_set_band_mode(int x){(void)x;return 0;}
static int esp_wifi_set_config(int i,wifi_config_t*c){(void)i;config=*c;++config_calls;return 0;}
static int esp_wifi_connect(void){++connects;return 0;}
static int esp_wifi_disconnect(void){return disconnect_error;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t*a){a->rssi=-50;return 0;}
static int esp_wifi_scan_start(wifi_scan_config_t*c,bool b){assert(!b&&c->scan_time.active.max==150);++scans;return scan_error;}
static int esp_wifi_scan_stop(void){++stops;return 0;}
static int esp_wifi_scan_get_ap_num(uint16_t*n){*n=scan_count;return records_error;}
static int esp_wifi_scan_get_ap_records(uint16_t*n,wifi_ap_record_t*a)
{++record_gets;if(records_error)return records_error;if(*n>scan_count)*n=scan_count;memcpy(a,records,*n*sizeof(*a));return 0;}
static int esp_wifi_clear_ap_list(void){++clears;return 0;}
static int nvs_open(const char*n,int m,int*h){(void)n;(void)m;*h=1;return 0;}
static int nvs_flash_init(void){return 0;}
static int nvs_get_blob(int h,const char*k,void*v,size_t*n)
{(void)h;(void)k;if(!stored_valid)return ESP_FAIL;assert(*n==98);memcpy(v,stored,98);return 0;}
static int nvs_get_str(int h,const char*k,char*v,size_t*n){(void)h;(void)k;(void)v;(void)n;return ESP_FAIL;}
static int nvs_set_blob(int h,const char*k,const void*v,size_t n)
{(void)h;(void)k;assert(n==98);++writes;if(!commit_error){memcpy(stored,v,n);stored_valid=true;}return 0;}
static int nvs_commit(int h){(void)h;++commits;return commit_error;}
static void nvs_close(int h){(void)h;}
static int gpio_get_level(int n){assert(n==54);return 1;}
static int bsp_usb_cdc_write(const char*s){snprintf(usb_line,sizeof(usb_line),"%s",s);return 0;}
'''


class HostedProductTests(unittest.TestCase):
    def test_complete_product_radio_owner(self):
        cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("host C compiler required")
        source = (ROOT / "main/bsp_wifi_hosted.c").read_text(encoding="utf-8")
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        tests = (ROOT / "tests/wifi_hosted_test.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "wifi.c"
            exe = Path(td) / "wifi.exe"
            path.write_text(DOUBLES + source + tests, encoding="utf-8")
            built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-unused-function", "-Wno-unused-variable",
                                    "-I", str(ROOT / "main"), str(path), "-o", str(exe)],
                                   capture_output=True, text=True, timeout=45)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout)

    def test_no_implicit_c5_reset_or_transport_restart(self):
        source = (ROOT / "main/bsp_wifi_hosted.c").read_text(encoding="utf-8")
        for forbidden in ("gpio_set_level(", "esp_hosted_deinit(", "esp_hosted_init(", "pulse_c5_en("):
            self.assertNotIn(forbidden, source)
        self.assertIn("esp_hosted_sdio_diagnostic_set_reset(false)", source)


if __name__ == "__main__":
    unittest.main()
