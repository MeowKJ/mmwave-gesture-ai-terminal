/* Development-board Hosted RPC acceptance, not product firmware or an OTA loader.
 * Hosted owns SDIO for the entire boot. No raw PROBE, C5RESET, GPIO6 or Flash writes.
 * The component constructor initializes Hosted; CONNECT is explicit and once/boot. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_sdio_diagnostic.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { IDLE, CONNECTING, READY, VERIFYING, FAILED } state_t;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static state_t state;
static esp_err_t last_error;
static unsigned rpc_pairs, verify_runs;
static const char *const names[] = {"IDLE", "CONNECTING", "READY", "VERIFYING", "FAILED"};

static void result(state_t next, esp_err_t error)
{
    portENTER_CRITICAL(&lock);
    state = next;
    last_error = error;
    portEXIT_CRITICAL(&lock);
}

static void status(void)
{
    portENTER_CRITICAL(&lock);
    state_t s = state;
    esp_err_t error = last_error;
    unsigned pairs = rpc_pairs, runs = verify_runs;
    portEXIT_CRITICAL(&lock);
    printf("MEO:HOSTED_STATUS state=%s error=0x%x rpc_pairs=%u verify_runs=%u "
           "source=CACHED gpio6=UNUSED flash_writes=0\n", names[s], (unsigned)error, pairs, runs);
    fflush(stdout);
}

static void info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_err_t error = esp_flash_get_size(NULL, &flash);
    printf("MEO:BRINGUP project=%s profile=hosted reset=%d revision=%u flash=%" PRIu32
           " flash_error=0x%x gpio6=UNUSED baseboard_io=UNUSED elf_sha=",
           app->project_name, (int)esp_reset_reason(), chip.revision, flash, (unsigned)error);
    for (unsigned i = 0; i < 32; ++i) printf("%02x", app->app_elf_sha256[i]);
    printf("\n");
    status();
}

static esp_err_t rpc_identity(unsigned run, unsigned sample)
{
    esp_hosted_coprocessor_fwver_t fw = {0};
    uint32_t chip_id = 0;
    char chip[16] = {0};
    esp_err_t error = esp_hosted_get_coprocessor_fwversion(&fw);
    if (error == ESP_OK) error = esp_hosted_get_cp_info(&chip_id, chip, sizeof(chip));
    if (error == ESP_OK && strcmp(chip, "esp32c5") != 0) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK) {
        portENTER_CRITICAL(&lock);
        ++rpc_pairs;
        portEXIT_CRITICAL(&lock);
    }
    printf("MEO:HOSTED_RPC run=%u sample=%u error=0x%x chip=%s chip_id=%" PRIu32
           " fw=%" PRIu32 ".%" PRIu32 ".%" PRIu32 " source=LIVE_RPC\n",
           run, sample, (unsigned)error, chip, chip_id, fw.major1, fw.minor1, fw.patch1);
    fflush(stdout);
    return error;
}

static void connect_worker(void *unused)
{
    (void)unused;
    esp_err_t error = esp_hosted_connect_to_slave();
    if (error == ESP_OK) error = rpc_identity(0, 0);
    result(error == ESP_OK ? READY : FAILED, error);
    printf("MEO:HOSTED_CONNECT passed=%u error=0x%x\n", error == ESP_OK, (unsigned)error);
    fflush(stdout);
    vTaskDelete(NULL);
}

static void verify_worker(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&lock);
    unsigned run = ++verify_runs;
    portEXIT_CRITICAL(&lock);
    esp_err_t error = ESP_OK;
    unsigned completed = 0;
    for (unsigned i = 0; i < 10; ++i) {
        error = rpc_identity(run, i);
        if (error != ESP_OK) break;
        ++completed;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    result(error == ESP_OK ? READY : FAILED, error);
    printf("MEO:HOSTED_VERIFY run=%u passed=%u samples=%u error=0x%x\n",
           run, error == ESP_OK && completed == 10, completed, (unsigned)error);
    fflush(stdout);
    vTaskDelete(NULL);
}

static void command(const char *line)
{
    if (!strcmp(line, "INFO")) { info(); return; }
    if (!strcmp(line, "STATUS")) { status(); return; }
    bool connect = !strcmp(line, "CONNECT"), verify = !strcmp(line, "VERIFY");
    if (!connect && !verify) {
        printf("MEO:COMMAND rejected=HOSTED_EXCLUSIVE_OWNER\n");
        return;
    }
    portENTER_CRITICAL(&lock);
    bool accepted = (connect && state == IDLE) || (verify && state == READY);
    if (accepted) state = connect ? CONNECTING : VERIFYING;
    portEXIT_CRITICAL(&lock);
    printf("MEO:HOSTED_ACCEPT command=%s accepted=%u\n", line, accepted);
    fflush(stdout);
    if (accepted && xTaskCreate(connect ? connect_worker : verify_worker,
                                "hosted_test", 8192, NULL, 2, NULL) != pdPASS)
        result(FAILED, ESP_ERR_NO_MEM);
}

void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));
    const uart_config_t cfg = {.baud_rate=115200, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 37, 38, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    esp_err_t loop_error = esp_event_loop_create_default();
    if (loop_error != ESP_OK && loop_error != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loop_error);
    /* One reset/init attempt, 10 s INIT budget, 1-bit/400 kHz. No reconnect loop. */
    esp_err_t error = esp_hosted_sdio_diagnostic_configure_ex(1, 400, 0, 10000);
    if (error != ESP_OK) result(FAILED, error);
    info();
    printf("MEO:COMMANDS INFO STATUS CONNECT VERIFY; no automatic C5 reset/Flash write\n");
    fflush(stdout);
    char line[64];
    size_t len = 0;
    bool overflow = false;
    int64_t heartbeat = esp_timer_get_time();
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100)) == 1) {
            if (ch == '\n' || ch == '\r') {
                if (len && !overflow) { line[len] = 0; command(line); }
                len = 0; overflow = false;
            } else if (len < sizeof(line)-1 && !overflow) line[len++] = (char)ch;
            else overflow = true;
        }
        if (esp_timer_get_time() - heartbeat > 5000000) {
            printf("MEO:ALIVE uptime_ms=%" PRId64 "\n", esp_timer_get_time()/1000);
            status();
            heartbeat = esp_timer_get_time();
        }
    }
}
