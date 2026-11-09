/* P4C5 reference wiring: SDIO D0=14 CLK=18 CMD=19. The user confirmed C5.
 * Only a zero-argument CMD5 query is sent. GPIO6/54, C5 boot/reset, PWR_HOLD,
 * screen and touch are never controlled here. A reply is NOT chip identity,
 * proof of ESP-Hosted firmware, a handshake, or proof that Wi-Fi is usable.
 *
 * IDF 6.1 handle API gives us an exclusive controller: never initialize or
 * deinitialize somebody else's legacy/global SDMMC host. One private worker,
 * one command with a 250ms event timeout, no retries or automatic reset.
 * Driver-level faults can exceed that timeout; USB/UI never wait for worker.
 */
#include "bsp_wifi_probe.h"
#include "bsp_usb.h"
#include "board_pins.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/sd_host.h"
#include "driver/sd_host_sdmmc.h"
#include "sd_protocol_defs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { IDLE, QUEUED, RUNNING, DONE } probe_state_t;
typedef struct {
    probe_state_t state;
    const char *result;
    char token[17];
    esp_err_t error, idle, cleanup;
    uint32_t ocr, elapsed_ms;
    bool blocked;
} probe_status_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static probe_status_t s_status = {.state = IDLE, .result = "NOT_RUN", .token = "none"};

static void report(const char *token)
{
    probe_status_t s;
    portENTER_CRITICAL(&s_lock);
    s = s_status;
    portEXIT_CRITICAL(&s_lock);
    static const char *const states[] = {"IDLE", "QUEUED", "RUNNING", "DONE"};
    char line[384];
    snprintf(line, sizeof(line),
             "MEO:WIFI token=%s probe=%s state=%s result=%s error=0x%x "
             "ocr=0x%08lx funcs=%u ready=%u elapsed_ms=%lu cleanup=0x%x "
             "blocked=%u hosted=NOT_VERIFIED wifi=UNAVAILABLE reset=0 idle=0x%x\n",
             token, s.token, states[s.state], s.result, (unsigned)s.error,
             (unsigned long)s.ocr, (unsigned)SD_IO_OCR_NUM_FUNCTIONS(s.ocr),
             (s.ocr & SD_IO_OCR_MEM_READY) != 0, (unsigned long)s.elapsed_ms,
             (unsigned)s.cleanup, s.blocked, (unsigned)s.idle);
    (void)bsp_usb_cdc_write(line);
}

bool bsp_wifi_probe_command(const char *line)
{
    if (!line || strncmp(line, "WIFI ", 5) != 0) return false;
    char cmd[12], token[17], extra;
    if (sscanf(line, "WIFI %11s %16s %c", cmd, token, &extra) != 2) return true;
    for (const char *p = token; *p; ++p) {
        if (!isalnum((unsigned char)*p)) return true;
    }
    if (strcmp(cmd, "STATUS") == 0) {
        report(token);
    } else if (strcmp(cmd, "PROBE") == 0) {
        bool accepted = false;
        portENTER_CRITICAL(&s_lock);
        if (!s_status.blocked && (s_status.state == IDLE || s_status.state == DONE)) {
            s_status = (probe_status_t){.state = QUEUED, .result = "PENDING"};
            memcpy(s_status.token, token, strlen(token) + 1);
            accepted = true;
        }
        portEXIT_CRITICAL(&s_lock);
        char reply[96];
        snprintf(reply, sizeof(reply), "MEO:WIFI_ACCEPT token=%s accepted=%u\n", token, accepted);
        (void)bsp_usb_cdc_write(reply);
    }
    return true;
}

static void probe_task(void *arg)
{
    (void)arg;
    probe_status_t s;
    portENTER_CRITICAL(&s_lock);
    s = s_status;
    portEXIT_CRITICAL(&s_lock);
    int64_t start = esp_timer_get_time();
    sd_host_ctlr_handle_t ctlr = NULL;
    sd_host_slot_handle_t slot = NULL;
    bool pins_attempted = false;
    const sd_host_sdmmc_cfg_t host_cfg = {.event_queue_items = 4, .dma_desc_num = 4};
    s.error = sd_host_create_sdmmc_controller(&host_cfg, &ctlr);
    if (s.error != ESP_OK) {
        s.result = s.error == ESP_ERR_NOT_FOUND ? "HOST_BUSY" : "HOST_ERROR";
        /* Some IDF 6.1 failure paths can leave an unreturned controller behind.
         * Do not retry/reset to recover an unknown ownership state. */
        s.blocked = true;
        goto finish;
    }
    const sd_host_slot_sdmmc_init_cfg_t slot_cfg = {
        .slot_id = 1, .sd_mode = SD_MODE_NORMAL,
        .io_config = {
            .width = 1, .clk_io = PIN_C5_SDIO_CLK, .cmd_io = PIN_C5_SDIO_CMD,
            .d0_io = PIN_C5_SDIO_D0,
            .d1_io = GPIO_NUM_NC, .d2_io = GPIO_NUM_NC, .d3_io = GPIO_NUM_NC,
            .d4_io = GPIO_NUM_NC, .d5_io = GPIO_NUM_NC,
            .d6_io = GPIO_NUM_NC, .d7_io = GPIO_NUM_NC,
            .cd_io = GPIO_NUM_NC, .wp_io = GPIO_NUM_NC,
        },
        /* Debug fallback only, not evidence of adequate external pull-ups. */
        .slot_flags.internal_pullup = true,
    };
    pins_attempted = true;
    s.error = sd_host_sdmmc_controller_add_slot(ctlr, &slot_cfg, &slot);
    if (s.error != ESP_OK) {
        s.result = "HOST_ERROR";
        goto cleanup;
    }
    /* New driver starts at 400kHz, single-bit mode. No card_init(): that
     * performs IO reset and other state-changing commands.
     * CMD0 (GO_IDLE_STATE, no response) is the standard bus-idle preamble a
     * real card expects after power-up; it also clocks the command line 48
     * times, which covers the 74-clock requirement of the SD physical layer.
     * It changes nothing on a device that is not an SDIO card. It is sent once
     * and its own error is reported separately, never retried. */
    sdmmc_command_t idle = {
        .opcode = MMC_GO_IDLE_STATE, .arg = 0,
        .flags = SCF_CMD_BC | SCF_RSP_R0, .timeout_ms = 100,
    };
    s.idle = sd_host_slot_do_transaction(slot, &idle);
    /* CMD5(arg=0) asks for IO operating conditions only, no voltage selection. */
    sdmmc_command_t cmd = {
        .opcode = SD_IO_SEND_OP_COND, .arg = 0,
        .flags = SCF_CMD_BCR | SCF_RSP_R4, .timeout_ms = 250,
    };
    s.error = sd_host_slot_do_transaction(slot, &cmd);
    if (s.error == ESP_OK && cmd.error != ESP_OK) s.error = cmd.error;
    if (s.error == ESP_OK) {
        s.ocr = MMC_R4(cmd.response);
        bool plausible = s.ocr != UINT32_MAX &&
                         SD_IO_OCR_NUM_FUNCTIONS(s.ocr) > 0 && (s.ocr & SD_OCR_VOL_MASK) != 0;
        s.result = plausible ? "SDIO_R4" : "INVALID_R4";
        if (!plausible) s.error = ESP_ERR_INVALID_RESPONSE;
    } else {
        s.result = s.error == ESP_ERR_TIMEOUT ? "NO_RESPONSE" : "TRANSPORT_ERROR";
    }
cleanup:
    if (slot) s.cleanup = sd_host_remove_slot(slot);
    /* Never delete a controller if its slot failed to detach. */
    if (s.cleanup == ESP_OK) s.cleanup = sd_host_del_controller(ctlr);
    if (pins_attempted && s.cleanup == ESP_OK) {
        const gpio_num_t pins[] = {PIN_C5_SDIO_D0, PIN_C5_SDIO_CLK, PIN_C5_SDIO_CMD};
        for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
            esp_err_t err = gpio_reset_pin(pins[i]);
            if (s.cleanup == ESP_OK) s.cleanup = err;
        }
    }
    if (s.cleanup != ESP_OK) s.blocked = true;
finish:
    s.state = DONE;
    s.elapsed_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    portENTER_CRITICAL(&s_lock);
    s_status = s;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

void bsp_wifi_probe_service(void)
{
    bool start = false;
    portENTER_CRITICAL(&s_lock);
    if (s_status.state == QUEUED) {
        s_status.state = RUNNING;
        start = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (start && xTaskCreate(probe_task, "wifi_probe", 4096, NULL, 1, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_status.state = DONE;
        s_status.result = "TASK_ERROR";
        s_status.error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_lock);
    }
}
