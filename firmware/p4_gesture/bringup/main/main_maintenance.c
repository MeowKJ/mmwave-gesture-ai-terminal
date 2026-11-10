/* Development-board UART -> Hosted SDIO app-only maintenance.
 * No strap writes, bootloader/table writes or product peripherals. ATTACH never
 * resets C5; required to health-check an OTA candidate before confirming it. */
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_hosted_sdio_diagnostic.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "mbedtls/platform_util.h"
#include "wifi_diagnostic.h"
#include "../../../shared/c5_maintenance_protocol.h"

enum { IDLE, READY, WRITING, SEALED, ACTIVATED, FAILED };
static unsigned state, offset, total;
static char token[17]="none";
static uint8_t expected_digest[32], target_elf[32];
static psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
static QueueHandle_t responses;
static EventGroupHandle_t rpc_events;
#define RPC_READY_BIT BIT0
static uint32_t seq;
static meo_c5_response_t last_c5;
static bool c5_valid;
static const char *const states[]={"IDLE","READY","WRITING","SEALED","ACTIVATED","FAILED"};

static void hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id==ESP_HOSTED_EVENT_CP_INIT) xEventGroupSetBits(rpc_events,RPC_READY_BIT);
}

static void hex(const uint8_t *p, size_t n) { for (size_t i=0;i<n;i++) printf("%02x",p[i]); }
static bool unhex(const char *s, uint8_t *p, size_t n)
{
    if (strlen(s)!=n*2) return false;
    for (size_t i=0;i<n;i++) {
        if (!isxdigit((unsigned char)s[i*2]) || !isxdigit((unsigned char)s[i*2+1])) return false;
        char b[3]={s[i*2],s[i*2+1],0}; p[i]=(uint8_t)strtoul(b,NULL,16);
    }
    return true;
}
static void status(void)
{
    printf("MEO:MAINT_STATUS state=%s token=%s offset=%u total=%u source=CACHED\n",
           states[state],token,offset,total); fflush(stdout);
}
static void info(void)
{
    printf("MEO:BRINGUP project=%s profile=maintenance reset=%d elf_sha=",
           esp_app_get_description()->project_name,(int)esp_reset_reason());
    hex(esp_app_get_description()->app_elf_sha256,32); printf("\n"); status();
}
static void receive(uint32_t id, const uint8_t *p, size_t n, void *ctx)
{
    (void)ctx;
    if (id!=MEO_C5_MAINT_MSG || !p || n!=sizeof(meo_c5_response_t)) return;
    meo_c5_response_t r; memcpy(&r,p,sizeof(r));
    if (r.magic==MEO_C5_MAINT_MAGIC && r.version==MEO_C5_MAINT_VERSION)
        (void)xQueueOverwrite(responses,&r);
}
static esp_err_t call(unsigned op, unsigned arg0, unsigned arg1, const uint8_t *elf)
{
    meo_c5_request_t req={.magic=MEO_C5_MAINT_MAGIC,.version=MEO_C5_MAINT_VERSION,
        .op=op,.seq=++seq,.boot_id=c5_valid ? last_c5.boot_id : 0,.arg0=arg0,.arg1=arg1};
    if (elf) memcpy(req.elf_sha,elf,32);
    meo_c5_response_t r;
    while (xQueueReceive(responses,&r,0)==pdTRUE) {}
    esp_err_t error=esp_hosted_send_custom_data(MEO_C5_MAINT_MSG,(const uint8_t *)&req,sizeof(req));
    int64_t deadline=esp_timer_get_time()+5000000;
    bool got=false;
    while (error==ESP_OK && esp_timer_get_time()<deadline) {
        if (xQueueReceive(responses,&r,pdMS_TO_TICKS(100))==pdTRUE && r.seq==req.seq && r.op==op) {
            last_c5=r; c5_valid=true; got=true; error=(esp_err_t)r.error; break;
        }
    }
    if (error==ESP_OK && !got) error=ESP_ERR_TIMEOUT;
    printf("MEO:C5_MAINT op=%u seq=%lu received=%u error=0x%x",op,(unsigned long)req.seq,got,(unsigned)error);
    if (got) {
        printf(" boot=%08lx reset=%lu slot=0x%lx boot_slot=0x%lx ota_state=%lu flags=0x%lx "
               "pin_mode=%lu lease_ms=%lu pin_level=%lu uptime_ms=%lu elf_sha=",
               (unsigned long)r.boot_id,(unsigned long)r.reset_reason,(unsigned long)r.slot_address,
               (unsigned long)r.boot_address,(unsigned long)r.ota_state,(unsigned long)r.flags,
               (unsigned long)r.pin_mode,(unsigned long)r.lease_ms,(unsigned long)r.pin_level,
               (unsigned long)r.uptime_ms);
        hex(r.elf_sha,32);
    }
    printf("\n"); fflush(stdout); return error;
}
static esp_err_t identity(void)
{
    uint32_t chip_id=0; char chip[16]={0}; esp_hosted_coprocessor_fwver_t fw={0};
    esp_err_t error=esp_hosted_get_coprocessor_fwversion(&fw);
    if (error==ESP_OK) error=esp_hosted_get_cp_info(&chip_id,chip,sizeof(chip));
    if (error==ESP_OK && (strcmp(chip,"esp32c5") || chip_id!=23)) error=ESP_ERR_INVALID_RESPONSE;
    printf("MEO:HOSTED_ID error=0x%x chip=%s chip_id=%lu fw=%lu.%lu.%lu source=LIVE_RPC\n",
           (unsigned)error,chip,(unsigned long)chip_id,(unsigned long)fw.major1,
           (unsigned long)fw.minor1,(unsigned long)fw.patch1); fflush(stdout); return error;
}
static void reply(const char *op, esp_err_t error)
{
    printf("MEO:MAINT_REPLY op=%s token=%s error=0x%x state=%s offset=%u total=%u\n",
           op,token,(unsigned)error,states[state],offset,total); fflush(stdout);
}
static bool valid_token(const char *s)
{
    if (!*s || strlen(s)>16) return false;
    for (;*s;s++) if (!isalnum((unsigned char)*s)) return false;
    return true;
}
static void command(char *line)
{
    if (!strncmp(line,"NET_",4)) {
        char op[24]={0};
        (void)sscanf(line,"%23s",op);
        esp_err_t error=state==READY ? wifi_diagnostic_command(line) : ESP_ERR_INVALID_STATE;
        reply(op,error);
        return;
    }
    if (!strcmp(line,"INFO")) { info(); return; }
    if (!strcmp(line,"STATUS")) { status(); return; }
    if (!strcmp(line,"P4RESET")) {
        printf("MEO:P4RESET accepted=1 c5_reset=0\n"); fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250)); esp_restart();
    }
    if (!strcmp(line,"CONNECT") || !strcmp(line,"ATTACH")) {
        esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==IDLE) {
            error=esp_hosted_sdio_diagnostic_configure_ex(1,400,0,10000);
            if (error==ESP_OK) error=esp_hosted_sdio_diagnostic_set_reset(!strcmp(line,"CONNECT"));
            xEventGroupClearBits(rpc_events,RPC_READY_BIT);
            if (error==ESP_OK) error=esp_hosted_connect_to_slave();
            /* Transport INIT/TX_ACTIVE precedes the RPC ESPInit event. A live
             * reattach can take >1s to drain old stream data: do not send the
             * first identity request into that window. No request retries or
             * C5 reset is hidden in this readiness wait. */
            if (error==ESP_OK && !(xEventGroupWaitBits(rpc_events,RPC_READY_BIT,
                    pdFALSE,pdTRUE,pdMS_TO_TICKS(5000))&RPC_READY_BIT)) error=ESP_ERR_TIMEOUT;
            printf("MEO:RPC_READY error=0x%x source=CP_INIT\n",(unsigned)error); fflush(stdout);
            if (error==ESP_OK) error=identity();
            state=error==ESP_OK ? READY : FAILED;
        }
        reply(line,error); return;
    }
    if (!strcmp(line,"C5INFO") || !strcmp(line,"VERIFY")) {
        esp_err_t error=state==READY ? ESP_OK : ESP_ERR_INVALID_STATE;
        if (error==ESP_OK && !strcmp(line,"VERIFY")) error=identity();
        if (error==ESP_OK) error=call(MEO_C5_INFO,0,0,NULL);
        reply(line,error); return;
    }
    char a[129],b[129],c[129],d[129],extra;
    unsigned number=0;
    if (sscanf(line,"CONFIRM %128s %c",a,&extra)==1) {
        uint8_t elf[32]; esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==READY && unhex(a,elf,32) && c5_valid && !memcmp(elf,last_c5.elf_sha,32))
            error=call(MEO_C5_CONFIRM,0,0,elf);
        reply("CONFIRM",error); return;
    }
    if (sscanf(line,"PIN %128s %u %c",a,&number,&extra)==2) {
        unsigned mode=!strcmp(a,"HIZ") ? MEO_C5_HIZ : !strcmp(a,"UP") ? MEO_C5_PULLUP :
            !strcmp(a,"DOWN") ? MEO_C5_PULLDOWN : !strcmp(a,"LOW") ? MEO_C5_DRIVE_LOW :
            !strcmp(a,"HIGH") ? MEO_C5_DRIVE_HIGH : UINT32_MAX;
        esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==READY && c5_valid && mode<=MEO_C5_DRIVE_HIGH) {
            /* This headless profile never initializes audio. Product integration
             * must stop/delete I2S + mute codec before executing this handover. */
            const gpio_config_t cfg={.pin_bit_mask=1ULL<<6,.mode=GPIO_MODE_INPUT,
                .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
            error=gpio_config(&cfg);
            if (error==ESP_OK) error=call(MEO_C5_PIN,mode,number,NULL);
            vTaskDelay(pdMS_TO_TICKS(20));
            printf("MEO:P4_PIN gpio=6 mode=INPUT level=%d c5_error=0x%x\n",gpio_get_level(6),(unsigned)error);
        }
        reply("PIN",error); return;
    }
    if (sscanf(line,"OTA_BEGIN %128s %u %128s %128s %c",a,&number,b,c,&extra)==4) {
        esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==READY && valid_token(a) && number>=288 && number<=0x1e0000 &&
            unhex(b,expected_digest,32) && unhex(c,target_elf,32)) {
            error=call(MEO_C5_INFO,0,0,NULL);
            if (error==ESP_OK && (!(last_c5.flags&MEO_C5_ROLLBACK) ||
                last_c5.ota_state!=2 || last_c5.pin_mode!=MEO_C5_HIZ ||
                last_c5.slot_address!=last_c5.boot_address)) error=ESP_ERR_INVALID_STATE;
            if (error==ESP_OK) error=esp_hosted_slave_ota_begin();
            if (error==ESP_OK) {
                strcpy(token,a); total=number; offset=0;
                (void)psa_hash_abort(&sha);
                error=psa_hash_setup(&sha,PSA_ALG_SHA_256)==PSA_SUCCESS ? ESP_OK : ESP_FAIL;
                state=error==ESP_OK ? WRITING : FAILED;
            }
        }
        reply("OTA_BEGIN",error); return;
    }
    /* At most 512 bytes per stop-and-wait UART chunk; no pipelining/retry of
     * unacknowledged Hosted WRITE (upstream RPC has no offset or sequence). */
    if (!strncmp(line,"OTA_WRITE ",10)) {
        char *save=NULL,*op=strtok_r(line," ",&save),*t=strtok_r(NULL," ",&save),
             *off=strtok_r(NULL," ",&save),*payload=strtok_r(NULL," ",&save),*tail=strtok_r(NULL," ",&save);
        (void)op; esp_err_t error=ESP_ERR_INVALID_ARG;
        uint8_t chunk[512]; size_t n=payload ? strlen(payload)/2 : 0;
        char *end=NULL; unsigned long supplied=off ? strtoul(off,&end,10) : UINT32_MAX;
        if (state==WRITING && t && !strcmp(t,token) && off && *off && end && !*end &&
            supplied==offset && !tail && n>0 && n<=sizeof(chunk) && n<=total-offset && unhex(payload,chunk,n)) {
            error=ESP_OK;
            if (!offset && (n<288 || chunk[0]!=0xe9 || chunk[12]!=23 || chunk[13]!=0 ||
                memcmp(chunk+32,"\x32\x54\xcd\xab",4) || memcmp(chunk+176,target_elf,32)))
                error=ESP_ERR_INVALID_RESPONSE;
            if (error==ESP_OK) error=esp_hosted_slave_ota_write(chunk,n);
            if (error==ESP_OK) error=psa_hash_update(&sha,chunk,n)==PSA_SUCCESS ? ESP_OK : ESP_FAIL;
            if (error==ESP_OK) offset+=n;
            else state=FAILED;
        }
        reply("OTA_WRITE",error); return;
    }
    if (sscanf(line,"OTA_END %128s %c",d,&extra)==1) {
        esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==WRITING && !strcmp(d,token) && offset==total) {
            uint8_t digest[32]; size_t digest_size=0;
            error=psa_hash_finish(&sha,digest,sizeof(digest),&digest_size)==PSA_SUCCESS && digest_size==32 ? ESP_OK : ESP_FAIL;
            if (error==ESP_OK && memcmp(digest,expected_digest,32)) error=ESP_ERR_INVALID_CRC;
            if (error==ESP_OK) error=esp_hosted_slave_ota_end();
            state=error==ESP_OK ? SEALED : FAILED;
        }
        reply("OTA_END",error); return;
    }
    if (sscanf(line,"OTA_ACTIVATE %128s %c",d,&extra)==1) {
        esp_err_t error=ESP_ERR_INVALID_STATE;
        if (state==SEALED && !strcmp(d,token)) {
            state=ACTIVATED; /* On an ambiguous/lost ACK, never blindly repeat. */
            error=esp_hosted_slave_ota_activate();
            if (error!=ESP_OK) state=FAILED;
        }
        reply("OTA_ACTIVATE",error); return;
    }
    reply("UNKNOWN",ESP_ERR_NOT_SUPPORTED);
}

void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,8192,0,0,NULL,0));
    const uart_config_t cfg={.baud_rate=CONFIG_ESP_CONSOLE_UART_BAUDRATE,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,
        .stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0,&cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0,37,38,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    esp_err_t error=esp_event_loop_create_default();
    if (error!=ESP_OK && error!=ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(error);
    responses=xQueueCreate(1,sizeof(meo_c5_response_t));
    rpc_events=xEventGroupCreate();
    if (!responses || !rpc_events) abort();
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HOSTED_EVENT,ESP_HOSTED_EVENT_CP_INIT,hosted_event,NULL));
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_register_custom_callback(MEO_C5_MAINT_MSG,receive,NULL));
    if (psa_crypto_init()!=PSA_SUCCESS) abort();
    seq=esp_random();
    info();
    static char line[1400]; size_t len=0; bool overflow=false;
    int64_t heartbeat=esp_timer_get_time();
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0,&ch,1,pdMS_TO_TICKS(100))==1) {
            if (ch=='\n' || ch=='\r') {
                if (len && !overflow) { line[len]=0; command(line); }
                else if (overflow) reply("OVERFLOW",ESP_ERR_INVALID_SIZE);
                mbedtls_platform_zeroize(line,sizeof(line));
                len=0; overflow=false;
            } else if (len<sizeof(line)-1 && !overflow) line[len++]=(char)ch;
            else overflow=true;
        }
        if (esp_timer_get_time()-heartbeat>5000000) {
            printf("MEO:ALIVE uptime_ms=%" PRId64 "\n",esp_timer_get_time()/1000); fflush(stdout);
            heartbeat=esp_timer_get_time();
        }
    }
}
