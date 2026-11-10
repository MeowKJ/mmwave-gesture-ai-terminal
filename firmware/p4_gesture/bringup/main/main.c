/* Headless module bring-up. No display, PSRAM, audio, radar or baseboard GPIOs.
 * SDIO initialization changes card/CCCR configuration; this is not a passive probe.
 * Never owns SDIO concurrently with Hosted or esp-serial-flasher. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static unsigned probes;
static bool owner_stuck;
static void hex(const uint8_t *p, size_t n)
{
    for (size_t i=0; i<n; i++) printf("%02x", p[i]);
}

static void info(void)
{
    const esp_app_desc_t *app=esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash=0;
    esp_err_t flash_err=esp_flash_get_size(NULL, &flash);
    printf("MEO:BRINGUP project=%s reset=%d revision=%u flash=%" PRIu32 " flash_error=0x%x "
           "probes=%u owner_stuck=%u gpio6=UNUSED baseboard_io=UNUSED elf_sha=",
           app->project_name, (int)esp_reset_reason(), chip.revision, flash,
           (unsigned)flash_err, probes, owner_stuck);
    hex(app->app_elf_sha256, 32);
    printf("\n");
    fflush(stdout);
}

static esp_err_t read_regs(sdmmc_card_t *card, unsigned fn, unsigned addr, uint8_t *p, size_t n)
{
    for (size_t i=0; i<n; i++) {
        esp_err_t err=sdmmc_io_read_byte(card, fn, addr+i, &p[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void probe(void)
{
    if (owner_stuck) { printf("MEO:SDIO rejected=cleanup_failed\n"); return; }
    unsigned op=++probes;
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();
    host.slot=SDMMC_HOST_SLOT_1;
    host.flags=SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz=400;
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width=1;
    slot.clk=GPIO_NUM_18; slot.cmd=GPIO_NUM_19;
    slot.d0=GPIO_NUM_14; slot.d1=GPIO_NUM_15;
    slot.d2=GPIO_NUM_16; slot.d3=GPIO_NUM_17;
    slot.flags=SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t card={0};
    esp_err_t err=sdmmc_host_init();
    bool initialized=err == ESP_OK;
    printf("MEO:SDIO op=%u stage=HOST error=0x%x width=1 khz=400 c5_reset=0\n", op, (unsigned)err);
    if (err == ESP_OK) err=sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) err=sdmmc_card_init(&host, &card);
    printf("MEO:SDIO op=%u stage=CARD error=0x%x is_sdio=%u funcs=%u\n",
           op, (unsigned)err, card.is_sdio, card.num_io_functions);
    if (err == ESP_OK && !card.is_sdio) err=ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        uint8_t ptr[3]={0}, cis[64]={0};
        err=read_regs(&card, 0, 9, ptr, sizeof(ptr));
        unsigned address=ptr[0] | ((unsigned)ptr[1]<<8) | ((unsigned)ptr[2]<<16);
        if (err == ESP_OK && (!address || address > 0x1ffff-sizeof(cis))) err=ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) err=read_regs(&card, 0, address, cis, sizeof(cis));
        printf("MEO:CIS op=%u error=0x%x address=0x%x bytes=%u raw=", op, (unsigned)err,
               address, err == ESP_OK ? (unsigned)sizeof(cis) : 0);
        if (err == ESP_OK) hex(cis, sizeof(cis));
        printf("\n");
        if (err == ESP_OK) {
            unsigned vendor=0, device=0; bool found=false;
            for (size_t pos=0; pos<sizeof(cis);) {
                unsigned code=cis[pos++];
                if (code == 0xff) break;
                if (!code) continue;
                if (pos == sizeof(cis)) break;
                unsigned len=cis[pos++];
                if (len > sizeof(cis)-pos) break;
                if (code == 0x20 && len == 4) {
                    vendor=cis[pos] | ((unsigned)cis[pos+1]<<8);
                    device=cis[pos+2] | ((unsigned)cis[pos+3]<<8);
                    found=true; break;
                }
                pos+=len;
            }
            printf("MEO:CIS_ID op=%u valid=%u vendor=0x%04x device=0x%04x "
                   "rom_c5_identity=%u hosted=NOT_VERIFIED\n", op, found, vendor, device,
                   found && vendor == 0x0092 && device == 0x1017);
        }
        /* Only read shared state. No FIFO access, IRQ clearing or application writes. */
        for (unsigned sample=0; sample<5 && err == ESP_OK; sample++) {
            uint8_t data[12]={0};
            err=read_regs(&card, 1, 0x6c, data, sizeof(data));
            printf("MEO:SHARED op=%u sample=%u error=0x%x bytes=%u raw=", op, sample,
                   (unsigned)err, err == ESP_OK ? (unsigned)sizeof(data) : 0);
            if (err == ESP_OK) hex(data, sizeof(data));
            printf("\n"); fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    esp_err_t cleanup=initialized ? sdmmc_host_deinit() : ESP_OK;
    owner_stuck=cleanup != ESP_OK;
    printf("MEO:SDIO_DONE op=%u error=0x%x cleanup=0x%x hosted=NOT_VERIFIED\n",
           op, (unsigned)err, (unsigned)cleanup);
    fflush(stdout);
}

static void command(const char *line)
{
    if (!strcmp(line,"INFO")) info();
    else if (!strcmp(line,"PROBE")) probe();
    else if (!strcmp(line,"C5RESET")) {
        if (owner_stuck) { printf("MEO:C5_RESET rejected=cleanup_failed\n"); return; }
        /* Module-internal P4 GPIO54 -> C5 EN. No strap control, no GPIO6. */
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_54, 1));
        ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_54, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_54, 0));
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_54, 1));
        printf("MEO:C5_RESET completed=1 low_ms=20 straps=UNCONTROLLED\n"); fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000));
        probe();
    } else if (!strcmp(line,"P4RESET")) esp_restart();
    else printf("MEO:COMMAND error=unknown\n");
}

void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));
    const uart_config_t cfg={.baud_rate=115200, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 37, 38, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    info();
    printf("MEO:COMMANDS INFO PROBE C5RESET P4RESET\n"); fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1000));
    probe(); /* Once, with C5 untouched. No repeated reset loop. */
    char line[64]; size_t len=0; bool overflow=false;
    int64_t heartbeat=esp_timer_get_time();
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100)) == 1) {
            if (ch == '\n' || ch == '\r') {
                if (len && !overflow) { line[len]=0; command(line); }
                len=0; overflow=false;
            } else if (len < sizeof(line)-1 && !overflow) line[len++]=(char)ch;
            else overflow=true;
        }
        if (esp_timer_get_time()-heartbeat > 5000000) {
            printf("MEO:ALIVE uptime_ms=%" PRId64 "\n", esp_timer_get_time()/1000);
            fflush(stdout); heartbeat=esp_timer_get_time();
        }
    }
}
