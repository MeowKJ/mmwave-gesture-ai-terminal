/* C5 first provisioning via the module's internal SDIO bus.
 * Manual straps; optional explicit BOOTCONNECT resets C5 once on GPIO54.
 * The official-board profile never initializes radar/audio. GPIO6 is untouched.
 * ESP-Hosted runtime OTA is the subsequent upgrade path.
 */
#include "bsp_c5_loader.h"
#include "bsp_usb.h"
#include "board_pins.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "sdmmc_cmd.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_md5.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c5_payload_layout.h"
#include "../../c5_hosted/diagnostic/protocol.h"

extern const uint8_t payload_start[] asm("_binary_c5_payload_start");
extern const uint8_t payload_end[] asm("_binary_c5_payload_end");
static const char *TAG = "c5_loader";
static void port_deinit(esp_loader_port_t *base);
static esp_loader_t s_loader;
static bool s_connect_reset;
static bool s_reset_completed;
/* Cached P4 pad evidence only: not a C5-side waveform or boot-mode proof. */
static struct {
    char operation[17];
    bool valid;
    esp_err_t error;
    int before, low_begin, low_end, high_begin, high_end;
    uint32_t low_us;
    gpio_io_config_t config;
} s_reset_trace;
static struct {
    bool valid;
    uint32_t revision, strap, lp_sys, reset_cause;
    uint32_t gpio_in, gpio_enable, mux26, mux27, mux28;
} s_facts;
static struct {
    esp_loader_port_t base;
    sdmmc_host_t host;
    sdmmc_card_t card;
    int64_t deadline;
    bool host_owned;
    esp_err_t io_error;
} s_port;

typedef enum { IDLE, CONNECT_QUEUED, CONNECTING, CONNECTED, FLASH_QUEUED, WRITING, DONE, FAILED } state_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    state_t state;
    esp_loader_error_t error;
    const char *stage;
    char operation[17];
    unsigned chip;
    uint32_t written, total, flash_size;
    bool verified;
} s_status = {.state=IDLE, .stage="IDLE", .operation="none", .chip=ESP_UNKNOWN_CHIP};

/* Read-only evidence path. Flash/CONNECT cannot run while this cache is being
 * filled. USB requests fetch bounded cached pieces; lost replies never trigger
 * another flash read, reset or write. */
static struct {
    unsigned state; /* 0=none, 1=queued, 2=reading, 3=done, 4=failed */
    char operation[17];
    uint32_t offset, size;
    esp_loader_error_t error;
    char md5[33];
    uint8_t data[4096];
} s_read;

static struct {
    unsigned state; /* 0=idle, 1=queued, 2=running, 3=pass, 4=failed, 5=observed */
    char operation[17], elf_sha[65];
    const char *stage;
    esp_err_t error;
    unsigned samples, echoes, reset_reason;
    uint32_t heartbeat_first, heartbeat_last;
    bool reset_completed;
    bool observe;
} s_diag = {.stage="IDLE"};
static struct {
    uint8_t data[12];
    unsigned bytes;
    esp_err_t error;
    uint32_t uptime_ms;
} s_trace[MEO_C5_DIAG_SAMPLES];
static unsigned s_trace_count;

static bool valid_token(const char *token)
{
    if (!*token) return false;
    for (const char *p=token; *p; ++p) if (!isalnum((unsigned char)*p)) return false;
    return true;
}

static void read_report(const char *token)
{
    char line[240];
    static const char *const names[] = {"IDLE", "QUEUED", "READING", "DONE", "FAILED"};
    portENTER_CRITICAL(&s_lock);
    snprintf(line, sizeof(line), "MEO:C5_READ token=%s operation=%s state=%s offset=0x%08lx size=%lu error=%u md5=%s\n",
             token, *s_read.operation ? s_read.operation : "none", names[s_read.state],
             (unsigned long)s_read.offset, (unsigned long)s_read.size,
             (unsigned)s_read.error, *s_read.md5 ? s_read.md5 : "none");
    portEXIT_CRITICAL(&s_lock);
    (void)bsp_usb_cdc_write(line);
}

static void read_worker(void *arg)
{
    (void)arg;
    esp_loader_error_t err = esp_loader_flash_read(&s_loader, s_read.data, s_read.offset, s_read.size);
    char hex[33] = {0};
    if (err == ESP_LOADER_SUCCESS) {
        md5_context_t ctx;
        uint8_t digest[16];
        esp_rom_md5_init(&ctx);
        esp_rom_md5_update(&ctx, s_read.data, s_read.size);
        esp_rom_md5_final(digest, &ctx);
        for (unsigned i=0; i<16; ++i) snprintf(hex+i*2, 3, "%02x", digest[i]);
    } else {
        ESP_LOGE(TAG, "read offset=0x%08lx loader_error=%u io_error=0x%x; no retry/reset",
                 (unsigned long)s_read.offset, err, s_port.io_error);
        port_deinit(&s_port.base);
    }
    portENTER_CRITICAL(&s_lock);
    s_read.error = err;
    memcpy(s_read.md5, hex, sizeof(hex));
    s_read.state = err == ESP_LOADER_SUCCESS ? 3 : 4;
    if (err != ESP_LOADER_SUCCESS) {
        s_status.state = FAILED;
        s_status.error = err;
        s_status.stage = "READ_FAILED";
    }
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static esp_loader_error_t map_error(esp_err_t err)
{
    s_port.io_error = err;
    return err == ESP_OK ? ESP_LOADER_SUCCESS :
           err == ESP_ERR_TIMEOUT ? ESP_LOADER_ERROR_TIMEOUT : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t port_init_mode(bool minimal)
{
    s_port.host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    s_port.host.slot = SDMMC_HOST_SLOT_1;
    s_port.host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    s_port.host.max_freq_khz = 5000;
    if (minimal) {
        s_port.host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
        s_port.host.max_freq_khz = 400;
    }
    s_port.host.command_timeout_ms = 1000;
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) return map_error(err);
    s_port.host_owned = true;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = minimal ? 1 : 4;
    slot.clk = PIN_C5_SDIO_CLK;
    slot.cmd = PIN_C5_SDIO_CMD;
    slot.d0 = PIN_C5_SDIO_D0;
    slot.d1 = PIN_C5_SDIO_D1;
    slot.d2 = PIN_C5_SDIO_D2;
    slot.d3 = PIN_C5_SDIO_D3;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return map_error(sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot));
}

static esp_loader_error_t port_init(esp_loader_port_t *base)
{
    (void)base;
    return port_init_mode(false);
}

static void port_deinit(esp_loader_port_t *base)
{
    (void)base;
    free(s_port.card.host.dma_aligned_buffer);
    s_port.card.host.dma_aligned_buffer = NULL;
    if (s_port.host_owned) {
        esp_err_t err = sdmmc_host_deinit();
        s_port.host_owned = false;
        if (err != ESP_OK) s_port.io_error = err;
    }
}

static void no_reset(esp_loader_port_t *base) { (void)base; }
static void start_timer(esp_loader_port_t *base, uint32_t ms)
{
    (void)base;
    s_port.deadline = esp_timer_get_time() + (int64_t)ms * 1000;
}
static uint32_t remaining(esp_loader_port_t *base)
{
    (void)base;
    int64_t left = (s_port.deadline - esp_timer_get_time()) / 1000;
    return left > 0 ? (uint32_t)left : 0;
}
static void delay_ms(esp_loader_port_t *base, uint32_t ms)
{
    (void)base;
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}
static void port_log(esp_loader_port_t *base, esp_loader_log_level_t level, const char *fmt, va_list args)
{
    (void)base; (void)level;
    char text[256];
    vsnprintf(text, sizeof(text), fmt, args);
    ESP_LOGI(TAG, "%s", text);
}
static esp_loader_error_t card_init(esp_loader_port_t *base)
{
    (void)base;
    return map_error(sdmmc_card_init(&s_port.host, &s_port.card));
}
static esp_loader_error_t sdio_write(esp_loader_port_t *base, uint32_t fn, uint32_t addr,
                                     const uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)base;
    if (!timeout) return ESP_LOADER_ERROR_TIMEOUT;
    s_port.card.host.command_timeout_ms = timeout < 1000 ? timeout : 1000;
    return map_error(sdmmc_io_write_bytes(&s_port.card, fn, addr, data, size));
}
static esp_loader_error_t sdio_read(esp_loader_port_t *base, uint32_t fn, uint32_t addr,
                                    uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)base;
    if (!timeout) return ESP_LOADER_ERROR_TIMEOUT;
    s_port.card.host.command_timeout_ms = timeout < 1000 ? timeout : 1000;
    return map_error(sdmmc_io_read_bytes(&s_port.card, fn, addr, data, size));
}
static const esp_loader_port_ops_t s_ops = {
    .init=port_init, .deinit=port_deinit,
    .enter_bootloader=no_reset, .reset_target=no_reset,
    .start_timer=start_timer, .remaining_time=remaining, .delay_ms=delay_ms,
    .log=port_log, .sdio_write=sdio_write, .sdio_read=sdio_read, .sdio_card_init=card_init,
};

static void report(const char *token)
{
    char line[512];
    static const char *const names[] = {"IDLE","QUEUED","CONNECTING","CONNECTED","QUEUED","WRITING","DONE","FAILED"};
    portENTER_CRITICAL(&s_lock);
    snprintf(line, sizeof(line), "MEO:C5_LOADER token=%s operation=%s state=%s stage=%s error=%u "
             "chip=%u flash_size=%lu written=%lu total=%lu verified=%u payload=%s reset=%s\n",
             token, s_status.operation, names[s_status.state], s_status.stage, (unsigned)s_status.error,
             s_status.chip, (unsigned long)s_status.flash_size, (unsigned long)s_status.written,
             (unsigned long)s_status.total, s_status.verified, C5_PAYLOAD_SHA,
             s_connect_reset ? "once" : "never");
    portEXIT_CRITICAL(&s_lock);
    (void)bsp_usb_cdc_write(line);
}
static void stage(const char *name)
{
    portENTER_CRITICAL(&s_lock);
    s_status.stage = name;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t reset_c5_once(const char *operation)
{
    /* Never called as error recovery. This profile excludes radar/audio;
     * host additionally requires live UI radar=0 before each explicit request. */
    typeof(s_reset_trace) trace={.before=-1,.low_begin=-1,.low_end=-1,
        .high_begin=-1,.high_end=-1};
    snprintf(trace.operation,sizeof(trace.operation),"%s",operation);
    esp_err_t err=gpio_set_level(PIN_C5_RESET, 1);
    gpio_config_t cfg={.pin_bit_mask=1ULL << PIN_C5_RESET,
        .mode=GPIO_MODE_INPUT_OUTPUT, .pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE, .intr_type=GPIO_INTR_DISABLE};
    if (err == ESP_OK) err=gpio_config(&cfg);
    if (err == ESP_OK) err=gpio_get_io_config(PIN_C5_RESET,&trace.config);
    if (err == ESP_OK) {
        esp_rom_delay_us(1000); /* GPIO input synchronizer / pad settling */
        trace.before=gpio_get_level(PIN_C5_RESET);
        if (!trace.config.ie || !trace.config.oe || trace.config.od ||
            trace.config.fun_sel != PIN_FUNC_GPIO || trace.config.sig_out != SIG_GPIO_OUT_IDX ||
            trace.before != 1)
            err=ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        err=gpio_set_level(PIN_C5_RESET, 0);
        if (err == ESP_OK) {
            int64_t start=esp_timer_get_time();
            esp_rom_delay_us(1000);
            trace.low_begin=gpio_get_level(PIN_C5_RESET);
            vTaskDelay(pdMS_TO_TICKS(10));
            trace.low_end=gpio_get_level(PIN_C5_RESET);
            trace.low_us=(uint32_t)(esp_timer_get_time()-start);
            if (trace.low_begin != 0 || trace.low_end != 0) err=ESP_FAIL;
        }
    }
    /* Always release the shared reset line, including a failed LOW check.
     * Do not disable holds or change drive strength to force a result. */
    esp_err_t released=gpio_set_level(PIN_C5_RESET, 1);
    if (err == ESP_OK) err=released;
    esp_rom_delay_us(1000);
    trace.high_begin=gpio_get_level(PIN_C5_RESET);
    vTaskDelay(pdMS_TO_TICKS(5000));
    trace.high_end=gpio_get_level(PIN_C5_RESET);
    if (err == ESP_OK && (trace.high_begin != 1 || trace.high_end != 1)) err=ESP_FAIL;
    trace.error=err;
    trace.valid=true;
    portENTER_CRITICAL(&s_lock);
    s_reset_trace=trace;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

static void reset_trace_report(const char *token)
{
    typeof(s_reset_trace) trace;
    portENTER_CRITICAL(&s_lock);
    trace=s_reset_trace;
    portEXIT_CRITICAL(&s_lock);
    char line[400];
    snprintf(line,sizeof(line),"MEO:C5_RESET_TRACE token=%s operation=%s valid=%u error=%u before=%d low_begin=%d low_end=%d high_begin=%d high_end=%d low_us=%lu ie=%u oe=%u od=%u mux=%u signal=%u peripheral=%u sleep=%u scope=P4_PAD_ONLY\n",
        token,*trace.operation ? trace.operation : "none",trace.valid,(unsigned)trace.error,
        trace.before,trace.low_begin,trace.low_end,trace.high_begin,trace.high_end,
        (unsigned long)trace.low_us,trace.config.ie,trace.config.oe,trace.config.od,
        trace.config.fun_sel,trace.config.sig_out,trace.config.oe_ctrl_by_periph,trace.config.slp_sel);
    (void)bsp_usb_cdc_write(line);
}

static void diag_report(const char *token)
{
    static const char *const states[]={"IDLE","QUEUED","RUNNING","PASS","FAILED","OBSERVED"};
    char line[480];
    portENTER_CRITICAL(&s_lock);
    snprintf(line,sizeof(line),"MEO:C5_MINIMAL token=%s operation=%s state=%s stage=%s error=%u reset_completed=%u samples=%u echoes=%u hb_first=%lu hb_last=%lu reason=%u elf_sha=%s\n",
        token,*s_diag.operation ? s_diag.operation : "none",states[s_diag.state],s_diag.stage,
        (unsigned)s_diag.error,s_diag.reset_completed,s_diag.samples,s_diag.echoes,
        (unsigned long)s_diag.heartbeat_first,(unsigned long)s_diag.heartbeat_last,
        s_diag.reset_reason,*s_diag.elf_sha ? s_diag.elf_sha : "none");
    portEXIT_CRITICAL(&s_lock);
    (void)bsp_usb_cdc_write(line);
}

static void diag_stage(const char *name)
{
    portENTER_CRITICAL(&s_lock);
    s_diag.stage=name;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t diag_reg(unsigned pos, uint8_t *value)
{
    return sdmmc_io_read_byte(&s_port.card,1,MEO_C5_DIAG_REG_ADDR(pos),value);
}

static esp_err_t diag_read32(unsigned addr, uint32_t *value)
{
    uint8_t bytes[4];
    for (unsigned i=0; i<4; ++i) {
        esp_err_t err=sdmmc_io_read_byte(&s_port.card,1,addr+i,&bytes[i]);
        if (err != ESP_OK) return err;
    }
    *value=bytes[0] | (uint32_t)bytes[1]<<8 | (uint32_t)bytes[2]<<16 | (uint32_t)bytes[3]<<24;
    return ESP_OK;
}

static esp_err_t diag_snapshot(uint32_t *heartbeat, bool *valid, char hash[65], unsigned *reason)
{
    uint8_t reg[12]={0}, seq;
    unsigned bytes=0;
    esp_err_t read_err=ESP_OK;
    *valid=false;
    for (unsigned i=0; i<12; ++i) {
        read_err=diag_reg(i,&reg[i]);
        if (read_err != ESP_OK) break;
        ++bytes;
    }
    /* Preserve partial/error samples too. A failed CMD52 is never represented
     * as twelve successfully read zero bytes. No further IO when queried. */
    portENTER_CRITICAL(&s_lock);
    if (s_trace_count<MEO_C5_DIAG_SAMPLES) {
        unsigned i=s_trace_count++;
        memcpy(s_trace[i].data,reg,sizeof(reg));
        s_trace[i].bytes=bytes;
        s_trace[i].error=read_err;
        s_trace[i].uptime_ms=(uint32_t)(esp_timer_get_time()/1000);
    }
    portEXIT_CRITICAL(&s_lock);
    if (read_err != ESP_OK) return read_err;
    if (memcmp(reg,MEO_C5_DIAG_MAGIC,4) || reg[4] != MEO_C5_DIAG_VERSION || reg[5] != MEO_C5_DIAG_READY)
        return ESP_OK; /* No signature is not evidence of application execution. */
    esp_err_t err=diag_reg(7,&seq);
    if (err != ESP_OK) return err;
    if ((seq & 1) || seq != reg[7]) return ESP_OK;
    for (unsigned i=0; i<32; ++i) {
        uint8_t byte;
        err=diag_reg(32+i,&byte);
        if (err != ESP_OK) return err;
        snprintf(hash+i*2,3,"%02x",byte);
    }
    *heartbeat=reg[8] | (uint32_t)reg[9]<<8 | (uint32_t)reg[10]<<16 | (uint32_t)reg[11]<<24;
    *reason=reg[6];
    *valid=true;
    return ESP_OK;
}

static esp_err_t diag_test(void)
{
    diag_stage("RELEASE_OWNER");
    port_deinit(&s_port.base); /* exclusive handoff from ROM flasher to app SDIO */
    memset(&s_port.card,0,sizeof(s_port.card));
    esp_err_t err=ESP_OK;
    if (!s_diag.observe) {
        diag_stage("RESET_ONCE");
        err=reset_c5_once(s_diag.operation);
        if (err != ESP_OK) return err;
        portENTER_CRITICAL(&s_lock);
        s_diag.reset_completed=true;
        portEXIT_CRITICAL(&s_lock);
    }
    diag_stage("CARD_1BIT_400K");
    if (port_init_mode(true) != ESP_LOADER_SUCCESS) return s_port.io_error;
    err=sdmmc_card_init(&s_port.host,&s_port.card);
    if (err != ESP_OK) return err; /* no card-init retry */
    s_port.card.host.command_timeout_ms=1000;
    if (s_diag.observe) {
        diag_stage("OBSERVE_REGS");
        for (unsigned i=0; i<MEO_C5_DIAG_SAMPLES; ++i) {
            uint32_t heartbeat=0;
            bool valid=false;
            char hash[65]={0};
            unsigned reason=0;
            err=diag_snapshot(&heartbeat,&valid,hash,&reason);
            if (err != ESP_OK) return err;
            if (valid) {
                portENTER_CRITICAL(&s_lock);
                if (!s_diag.samples) s_diag.heartbeat_first=heartbeat;
                s_diag.heartbeat_last=heartbeat;
                memcpy(s_diag.elf_sha,hash,sizeof(hash));
                s_diag.reset_reason=reason;
                ++s_diag.samples;
                portEXIT_CRITICAL(&s_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        return ESP_OK; /* capture completed, NOT an application/echo PASS */
    }
    diag_stage("APP_SIGNATURE");
    int64_t end=esp_timer_get_time()+3000000;
    unsigned reason=0;
    char identity[65]={0};
    while (s_diag.samples < MEO_C5_DIAG_SAMPLES && esp_timer_get_time()<end) {
        uint32_t heartbeat=0;
        bool valid=false;
        char hash[65]={0};
        err=diag_snapshot(&heartbeat,&valid,hash,&reason);
        if (err != ESP_OK) return err;
        if (valid) {
            if (*identity && strcmp(identity,hash)) return ESP_ERR_INVALID_RESPONSE;
            if (s_diag.samples && (heartbeat < s_diag.heartbeat_last || reason != s_diag.reset_reason))
                return ESP_ERR_INVALID_STATE; /* app restarted during observation */
            memcpy(identity,hash,sizeof(identity));
            portENTER_CRITICAL(&s_lock);
            if (!s_diag.samples) {
                s_diag.heartbeat_first=heartbeat;
                memcpy(s_diag.elf_sha,hash,sizeof(hash));
                s_diag.reset_reason=reason;
            }
            s_diag.heartbeat_last=heartbeat;
            ++s_diag.samples;
            portEXIT_CRITICAL(&s_lock);
        }
        if (valid && s_diag.samples == 1) {
            diag_stage("HEARTBEAT");
            end=esp_timer_get_time()+10000000;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (s_diag.samples != MEO_C5_DIAG_SAMPLES || s_diag.heartbeat_last <= s_diag.heartbeat_first)
        return ESP_ERR_TIMEOUT;
    diag_stage("CMD53_ECHO");
    uint8_t tx[MEO_C5_DIAG_PACKET_SIZE] __attribute__((aligned(4)));
    uint8_t rx[MEO_C5_DIAG_PACKET_SIZE] __attribute__((aligned(4)));
    uint32_t rx_count=0;
    for (unsigned packet=0; packet<MEO_C5_DIAG_PACKETS; ++packet) {
        uint32_t token;
        err=diag_read32(0x44,&token);
        if (err != ESP_OK) return err;
        if ((((token>>16)-packet)&0xfff) == 0) return ESP_ERR_INVALID_STATE;
        esp_fill_random(tx,sizeof(tx)); /* fresh non-replayed challenge */
        tx[0]='M';tx[1]='5';tx[2]=(uint8_t)packet;tx[3]=MEO_C5_DIAG_VERSION;
        err=sdmmc_io_write_bytes(&s_port.card,1,0x1f800-sizeof(tx),tx,sizeof(tx));
        if (err != ESP_OK) return err; /* write exactly once */
        end=esp_timer_get_time()+2000000;
        uint32_t available=0;
        do {
            err=diag_read32(0x60,&available);
            if (err != ESP_OK) return err;
            available=(available-rx_count)&0xfffff;
            if (available) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (esp_timer_get_time()<end);
        if (available != sizeof(rx)) return available ? ESP_ERR_INVALID_SIZE : ESP_ERR_TIMEOUT;
        err=sdmmc_io_read_bytes(&s_port.card,1,0x1f800-sizeof(rx),rx,sizeof(rx));
        if (err != ESP_OK) return err;
        if (memcmp(tx,rx,sizeof(tx))) return ESP_ERR_INVALID_RESPONSE;
        rx_count+=sizeof(rx);
        portENTER_CRITICAL(&s_lock);
        s_diag.echoes=packet+1;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_OK;
}

static void diag_worker(void *arg)
{
    (void)arg;
    esp_err_t err=diag_test();
    port_deinit(&s_port.base);
    portENTER_CRITICAL(&s_lock);
    s_diag.error=err;
    s_diag.state=err == ESP_OK ? (s_diag.observe ? 5 : 3) : 4;
    if (err == ESP_OK) s_diag.stage=s_diag.observe ? "CAPTURED" : "VERIFIED";
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static void worker(void *arg)
{
    bool flash = (uintptr_t)arg != 0;
    esp_loader_error_t err = ESP_LOADER_SUCCESS;
    if (!flash) {
        if (s_connect_reset) {
            /* Explicit one-shot action, never a connection-failure fallback.
             * This profile excludes bsp_radar/audio from the linked app, and
             * the host must also check UI radar=0 before requesting it. */
            stage("RESET_ONCE");
            esp_err_t reset_err = reset_c5_once(s_status.operation);
            if (reset_err != ESP_OK) err = map_error(reset_err);
            portENTER_CRITICAL(&s_lock);
            s_reset_completed = err == ESP_LOADER_SUCCESS;
            portEXIT_CRITICAL(&s_lock);
        }
        if (err == ESP_LOADER_SUCCESS) stage("HOST_INIT");
        s_port.base.ops = &s_ops;
        if (err == ESP_LOADER_SUCCESS) err = esp_loader_init_sdio(&s_loader, &s_port.base);
        if (err == ESP_LOADER_SUCCESS) {
            stage("CONNECT");
            esp_loader_connect_args_t args = {.sync_timeout=2000, .trials=1};
            err = esp_loader_connect(&s_loader, &args);
        }
        if (err == ESP_LOADER_SUCCESS) {
            unsigned chip = esp_loader_get_target(&s_loader);
            portENTER_CRITICAL(&s_lock);
            s_status.chip = chip;
            portEXIT_CRITICAL(&s_lock);
            if (chip != ESP32C5_CHIP) err = ESP_LOADER_ERROR_UNSUPPORTED_CHIP;
        }
        if (err == ESP_LOADER_SUCCESS) {
            uint32_t size = 0;
            stage("FLASH_SIZE");
            err = esp_loader_flash_detect_size(&s_loader, &size);
            portENTER_CRITICAL(&s_lock);
            s_status.flash_size = size;
            portEXIT_CRITICAL(&s_lock);
            if (err == ESP_LOADER_SUCCESS && size < 0x400000) err = ESP_LOADER_ERROR_IMAGE_SIZE;
        }
        if (err == ESP_LOADER_SUCCESS) {
            /* ESP32-C5 IDF 6.1/esptool register definitions. Read-only whitelist;
             * no eFuse programming, force-download writes or GPIO2 access. */
            stage("READ_FACTS");
            uint32_t values[9] = {0};
            const uint32_t addresses[9] = {0x600b484c, 0x60091000, 0x600b1034, 0x600b0410,
                0x60091064, 0x60091034, 0x60090068, 0x6009006c, 0x60090070};
            /* Read only GPIO input/output-enable and IO MUX26..28. No pad
             * configuration changes; an input-disabled zero is NOT a LOW. */
            for (unsigned i=0; i<9 && err==ESP_LOADER_SUCCESS; ++i)
                err = esp_loader_read_register(&s_loader, addresses[i], &values[i]);
            portENTER_CRITICAL(&s_lock);
            s_facts.revision=values[0]; s_facts.strap=values[1];
            s_facts.lp_sys=values[2]; s_facts.reset_cause=values[3];
            s_facts.gpio_in=values[4]; s_facts.gpio_enable=values[5];
            s_facts.mux26=values[6]; s_facts.mux27=values[7]; s_facts.mux28=values[8];
            s_facts.valid=err==ESP_LOADER_SUCCESS;
            portEXIT_CRITICAL(&s_lock);
        }
    } else {
        /* Flash only the generated image regions. Keep NVS, PHY and
           factory partitions between/beyond them intact; no chip erase. */
        uint32_t total = 0;
        for (unsigned i = 0; i < sizeof(c5_regions)/sizeof(c5_regions[0]); ++i) {
            if (c5_regions[i].offset + c5_regions[i].size > (size_t)(payload_end-payload_start)) {
                err = ESP_LOADER_ERROR_IMAGE_SIZE;
                break;
            }
            total += c5_regions[i].size;
        }
        portENTER_CRITICAL(&s_lock);
        s_status.total = total;
        portEXIT_CRITICAL(&s_lock);
        int64_t end = esp_timer_get_time() + 180000000;
        for (unsigned i = 0; err == ESP_LOADER_SUCCESS && i < sizeof(c5_regions)/sizeof(c5_regions[0]); ++i) {
            esp_loader_flash_cfg_t cfg = {.offset=c5_regions[i].offset,
                .image_size=c5_regions[i].size, .block_size=4096, .skip_verify=false};
            stage("ERASE_REGION");
            err = esp_loader_flash_start(&s_loader, &cfg);
            for (uint32_t pos = 0; err == ESP_LOADER_SUCCESS && pos < cfg.image_size;) {
                if (esp_timer_get_time() >= end) { err = ESP_LOADER_ERROR_TIMEOUT; break; }
                uint32_t len = cfg.image_size-pos < cfg.block_size ? cfg.image_size-pos : cfg.block_size;
                stage("WRITE");
                err = esp_loader_flash_write(&s_loader, &cfg, payload_start+cfg.offset+pos, len);
                if (err == ESP_LOADER_SUCCESS) {
                    pos += len;
                    portENTER_CRITICAL(&s_lock);
                    s_status.written += len;
                    portEXIT_CRITICAL(&s_lock);
                }
                vTaskDelay(1);
            }
            if (err == ESP_LOADER_SUCCESS) {
                stage("VERIFY_MD5");
                err = esp_loader_flash_finish(&s_loader, &cfg);
            }
        }
    }
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "stage=%s loader_error=%u io_error=0x%x; no reset/retry", s_status.stage, err, s_port.io_error);
        port_deinit(&s_port.base);
    }
    portENTER_CRITICAL(&s_lock);
    s_status.error = err;
    s_status.state = err != ESP_LOADER_SUCCESS ? FAILED : flash ? DONE : CONNECTED;
    s_status.verified = flash && err == ESP_LOADER_SUCCESS;
    if (err == ESP_LOADER_SUCCESS) s_status.stage = flash ? "VERIFIED" : "READY";
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

bool bsp_c5_loader_command(const char *line)
{
    if (!line || strncmp(line, "C5 ", 3)) return false;
    char cmd[12], token[17], extra;
    if (!strncmp(line,"C5 DIAGTRACE ",13)) {
        unsigned index;
        if (sscanf(line,"C5 DIAGTRACE %16s %u %c",token,&index,&extra)!=2 || !valid_token(token)) return true;
        unsigned count,bytes=0;
        esp_err_t error=ESP_ERR_NOT_FOUND;
        uint32_t ms=0;
        uint8_t data[12]={0};
        char operation[17];
        portENTER_CRITICAL(&s_lock);
        count=s_trace_count;
        memcpy(operation,*s_diag.operation ? s_diag.operation : "none",*s_diag.operation ? strlen(s_diag.operation)+1 : 5);
        if (index<count) {
            bytes=s_trace[index].bytes;error=s_trace[index].error;
            ms=s_trace[index].uptime_ms;
            memcpy(data,s_trace[index].data,sizeof(data));
        }
        portEXIT_CRITICAL(&s_lock);
        char reply[256];
        int n=snprintf(reply,sizeof(reply),"MEO:C5_TRACE token=%s operation=%s index=%u count=%u bytes=%u error=%u uptime_ms=%lu raw=",
            token,operation,index,count,bytes,(unsigned)error,(unsigned long)ms);
        for (unsigned i=0; i<sizeof(data); ++i) n+=snprintf(reply+n,sizeof(reply)-n,"%02x",data[i]);
        snprintf(reply+n,sizeof(reply)-n,"\n");
        (void)bsp_usb_cdc_write(reply);
        return true;
    }
    if (sscanf(line,"C5 %11s %16s %c",cmd,token,&extra)==2 && valid_token(token)) {
        if (!strcmp(cmd,"RESETTRACE")) { reset_trace_report(token); return true; }
        if (!strcmp(cmd,"DIAGSTATUS")) { diag_report(token); return true; }
        if (!strcmp(cmd,"DIAGRUN") || !strcmp(cmd,"DIAGOBSERVE")) {
            bool accepted=false;
            portENTER_CRITICAL(&s_lock);
            if (C5_PAYLOAD_IS_DIAG && !s_diag.state &&
                (s_status.state==IDLE || s_status.state==DONE || s_status.state==CONNECTED) &&
                s_read.state!=1 && s_read.state!=2) {
                s_diag.state=1;
                s_diag.observe=!strcmp(cmd,"DIAGOBSERVE");
                memcpy(s_diag.operation,token,strlen(token)+1);
                accepted=true;
            }
            portEXIT_CRITICAL(&s_lock);
            char reply[96];
            snprintf(reply,sizeof(reply),"MEO:C5_MINIMAL_ACCEPT token=%s accepted=%u\n",token,accepted);
            (void)bsp_usb_cdc_write(reply);
            return true;
        }
    }
    if (s_diag.state) {
        /* No loader reads/writes/connects after app SDIO has taken ownership,
         * including after a failed run; recovery requires a new explicit boot. */
        if (sscanf(line,"C5 %11s %16s %c",cmd,token,&extra)==2 && valid_token(token)) {
            if (!strcmp(cmd,"STATUS")) report(token);
            else {
                char reply[96];
                snprintf(reply,sizeof(reply),"MEO:C5_LOADER_ACCEPT token=%s accepted=0\n",token);
                (void)bsp_usb_cdc_write(reply);
            }
        }
        return true;
    }
    if (!strncmp(line, "C5 READ ", 8)) {
        unsigned offset, size;
        if (sscanf(line, "C5 READ %16s %x %u %c", token, &offset, &size, &extra) != 3 || !valid_token(token)) return true;
        bool accepted = false;
        portENTER_CRITICAL(&s_lock);
        if (s_status.state == CONNECTED && s_read.state != 1 && s_read.state != 2 &&
            size > 0 && size <= sizeof(s_read.data) && offset < s_status.flash_size &&
            size <= s_status.flash_size-offset) {
            s_read.state = 1;
            s_read.offset = offset;
            s_read.size = size;
            s_read.error = ESP_LOADER_SUCCESS;
            s_read.md5[0] = 0;
            memcpy(s_read.operation, token, strlen(token)+1);
            accepted = true;
        }
        portEXIT_CRITICAL(&s_lock);
        char reply[96];
        snprintf(reply, sizeof(reply), "MEO:C5_READ_ACCEPT token=%s accepted=%u\n", token, accepted);
        (void)bsp_usb_cdc_write(reply);
        return true;
    }
    if (!strncmp(line, "C5 DATA ", 8)) {
        unsigned pos;
        if (sscanf(line, "C5 DATA %16s %u %c", token, &pos, &extra) != 2 || !valid_token(token)) return true;
        uint8_t data[128];
        unsigned size = 0;
        portENTER_CRITICAL(&s_lock);
        if (s_read.state == 3 && !strcmp(token, s_read.operation) && pos < s_read.size && pos % sizeof(data) == 0) {
            size = s_read.size-pos < sizeof(data) ? s_read.size-pos : sizeof(data);
            memcpy(data, s_read.data+pos, size);
        }
        portEXIT_CRITICAL(&s_lock);
        char reply[384];
        int n = snprintf(reply, sizeof(reply), "MEO:C5_DATA token=%s pos=%u size=%u data=", token, pos, size);
        for (unsigned i=0; i<size; ++i) n += snprintf(reply+n, sizeof(reply)-n, "%02x", data[i]);
        snprintf(reply+n, sizeof(reply)-n, "\n");
        (void)bsp_usb_cdc_write(reply);
        return true;
    }
    if (sscanf(line, "C5 %11s %16s %c", cmd, token, &extra) != 2) return true;
    for (const char *p=token; *p; ++p) if (!isalnum((unsigned char)*p)) return true;
    if (!strcmp(cmd, "STATUS")) { report(token); return true; }
    if (!strcmp(cmd, "READSTATUS")) { read_report(token); return true; }
    if (!strcmp(cmd, "FACTS")) {
        char reply[440];
        portENTER_CRITICAL(&s_lock);
        snprintf(reply,sizeof(reply),"MEO:C5_FACTS token=%s valid=%u rev_raw=0x%08lx strap=0x%08lx lp_sys=0x%08lx reset_cause=0x%08lx reset_completed=%u gpio_in=0x%08lx gpio_enable=0x%08lx mux26=0x%08lx mux27=0x%08lx mux28=0x%08lx\n",
            token,s_facts.valid,(unsigned long)s_facts.revision,(unsigned long)s_facts.strap,
            (unsigned long)s_facts.lp_sys,(unsigned long)s_facts.reset_cause,s_reset_completed,
            (unsigned long)s_facts.gpio_in,(unsigned long)s_facts.gpio_enable,
            (unsigned long)s_facts.mux26,(unsigned long)s_facts.mux27,(unsigned long)s_facts.mux28);
        portEXIT_CRITICAL(&s_lock);
        (void)bsp_usb_cdc_write(reply);
        return true;
    }
    bool accepted = false;
    portENTER_CRITICAL(&s_lock);
    if (((!strcmp(cmd, "CONNECT") || !strcmp(cmd, "BOOTCONNECT")) && s_status.state == IDLE) ||
        (!strcmp(cmd, "FLASH") && s_status.state == CONNECTED && s_read.state != 1 && s_read.state != 2)) {
        bool connect = strcmp(cmd,"FLASH") != 0;
        s_status.state = connect ? CONNECT_QUEUED : FLASH_QUEUED;
        if (connect) s_connect_reset = !strcmp(cmd,"BOOTCONNECT");
        memcpy(s_status.operation, token, strlen(token)+1);
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    char reply[96];
    snprintf(reply, sizeof(reply), "MEO:C5_LOADER_ACCEPT token=%s accepted=%u\n", token, accepted);
    (void)bsp_usb_cdc_write(reply);
    return true;
}

void bsp_c5_loader_service(void)
{
    int action = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_read.state == 1) { s_read.state=2; action=3; }
    if (s_status.state == CONNECT_QUEUED) { s_status.state=CONNECTING; action=1; }
    if (s_status.state == FLASH_QUEUED) { s_status.state=WRITING; action=2; }
    if (s_diag.state == 1) { s_diag.state=2; action=4; }
    portEXIT_CRITICAL(&s_lock);
    if (action && xTaskCreate(action==4 ? diag_worker : action==3 ? read_worker : worker, "c5_loader", 8192,
                             (void *)(uintptr_t)(action==2), 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_status.state=FAILED;
        s_status.stage="TASK_ERROR";
        s_status.error=ESP_LOADER_ERROR_FAIL;
        if (action==3) { s_read.state=4; s_read.error=ESP_LOADER_ERROR_FAIL; }
        if (action==4) { s_diag.state=4; s_diag.stage="TASK_ERROR"; s_diag.error=ESP_ERR_NO_MEM; }
        portEXIT_CRITICAL(&s_lock);
    }
}
