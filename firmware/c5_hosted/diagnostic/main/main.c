/* Minimal Flash-boot / SDIO execution proof. No Wi-Fi, BT, NVS or UART.
 * Buffer ownership follows IDF examples/peripherals/sdio/slave. */
#include "protocol.h"
#include "driver/sdio_slave.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static DMA_ATTR uint8_t buffers[4][MEO_C5_DIAG_BUFFER_SIZE];

static void fail(esp_err_t err)
{
    (void)sdio_slave_write_reg(5, 0x80);
    /* No software reboot loop: leave the signature/error stage observable. */
    (void)err;
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void check(esp_err_t err) { if (err != ESP_OK) fail(err); }

void app_main(void)
{
    /* C5 IO2 is tied to the P4 audio net. Never enable an output or pulls. */
    gpio_config_t io2 = {.pin_bit_mask=1ULL << 2, .mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&io2));
    sdio_slave_config_t cfg = {.timing=SDIO_SLAVE_TIMING_PSEND_PSAMPLE,
        .sending_mode=SDIO_SLAVE_SEND_PACKET, .send_queue_size=4,
        .recv_buffer_size=MEO_C5_DIAG_BUFFER_SIZE,
        .flags=SDIO_SLAVE_FLAG_INTERNAL_PULLUP | SDIO_SLAVE_FLAG_DEFAULT_SPEED};
    /* Failure before initialize is an acknowledged observation blind spot. */
    if (sdio_slave_initialize(&cfg) != ESP_OK) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (int i=0; i<4; ++i) check(sdio_slave_write_reg(i, MEO_C5_DIAG_MAGIC[i]));
    check(sdio_slave_write_reg(4, MEO_C5_DIAG_VERSION));
    check(sdio_slave_write_reg(5, 2));
    check(sdio_slave_write_reg(6, (uint8_t)esp_reset_reason()));
    const esp_app_desc_t *desc=esp_app_get_description();
    for (int i=0; i<32; ++i) check(sdio_slave_write_reg(32+i, desc->app_elf_sha256[i]));
    for (int i=0; i<4; ++i) {
        sdio_slave_buf_handle_t h=sdio_slave_recv_register_buf(buffers[i]);
        if (!h) fail(ESP_ERR_NO_MEM);
        check(sdio_slave_recv_load_buf(h));
    }
    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET);
    check(sdio_slave_start());
    check(sdio_slave_write_reg(5, MEO_C5_DIAG_READY));
    uint8_t seq=0;
    int64_t last=-1;
    for (;;) {
        int64_t now=esp_timer_get_time()/100000;
        if (now != last) {
            check(sdio_slave_write_reg(7, ++seq)); /* odd = updating */
            for (int i=0; i<4; ++i) check(sdio_slave_write_reg(8+i, (uint32_t)now >> (i*8)));
            check(sdio_slave_write_reg(7, ++seq)); /* even = committed */
            last=now;
        }
        sdio_slave_buf_handle_t h;
        uint8_t *data;
        size_t size;
        esp_err_t err=sdio_slave_recv(&h, &data, &size, 0);
        if (err == ESP_OK) {
            if (size != MEO_C5_DIAG_PACKET_SIZE) fail(ESP_ERR_INVALID_SIZE);
            check(sdio_slave_send_queue(data, size, h, 0));
        } else if (err != ESP_ERR_TIMEOUT) fail(err);
        void *finished;
        while ((err=sdio_slave_send_get_finished(&finished, 0)) == ESP_OK)
            check(sdio_slave_recv_load_buf(finished));
        if (err != ESP_ERR_TIMEOUT) fail(err);
        vTaskDelay(1);
    }
}
