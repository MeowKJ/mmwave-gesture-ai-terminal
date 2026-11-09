/* Read-only C5 pin survey plus a HIGH-only CHIP_PU release action.
 *
 * Why this exists: the product image never configures GPIO54, and the C5 did
 * not answer CMD5. Before blaming the chip, measure what the shared reset line
 * actually is at runtime.
 *
 * Safety rules kept in code:
 *  - GPIO54 is never driven LOW. Only HIGH (release) or input (no drive).
 *  - No automatic action: every operation needs an explicit USB command+nonce.
 *  - GPIO6 (module: C5 IO2) is only read as an input; this profile never calls
 *    bsp_audio_init(), so the pad is free, and it is restored to default after.
 *  - SDIO pins are borrowed only for the duration of a level read and then
 *    returned to their reset default; the SDIO controller is not involved.
 */
#include "bsp_c5_diag.h"

#include "board_pins.h"
#include "bsp_usb.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "meoave_c5";
static bool s_pin54_released;

static bool valid_token(const char *token)
{
    if (!token || !*token || strlen(token) > 16) return false;
    for (const char *p = token; *p; ++p) {
        if (!isalnum((unsigned char)*p)) return false;
    }
    return true;
}

/* Configure as input without driving the line, then read the pad. */
static int read_input(gpio_num_t pin, bool pullup)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) return -1;
    return gpio_get_level(pin);
}

/* Restore the pin to its power-on default (input, no pull). */
static void release_to_default(gpio_num_t pin)
{
    (void)gpio_reset_pin(pin);
}

static int pin54_level(void)
{
    if (s_pin54_released) {
        /* INPUT_OUTPUT keeps the input buffer on so this reads the real pad. */
        return gpio_get_level(PIN_C5_RESET);
    }
    return read_input(PIN_C5_RESET, false);
}

static void report_pins(const char *token)
{
    /* "np" = no internal pull (what the bus really rests at),
     * "pu" = internal pull-up (tells a driven/high line from a floating one). */
    int d0np = read_input(PIN_C5_SDIO_D0, false), d0pu = read_input(PIN_C5_SDIO_D0, true);
    int clknp = read_input(PIN_C5_SDIO_CLK, false), clkpu = read_input(PIN_C5_SDIO_CLK, true);
    int cmdnp = read_input(PIN_C5_SDIO_CMD, false), cmdpu = read_input(PIN_C5_SDIO_CMD, true);
    int d1np = read_input(PIN_C5_SDIO_D1, false);
    int d2np = read_input(PIN_C5_SDIO_D2, false);
    int d3np = read_input(PIN_C5_SDIO_D3, false);
    /* C5 IO2 (module figure: ESP32C5HF4 GPIO2 <-> ESP32P4 GPIO6). On this board
     * the same P4 pin is ES8389 I2S DOUT, and this profile never calls
     * bsp_audio_init(), so the pad is free. Read-only: three pulls tell a driven
     * line from a floating one; nothing is ever driven, then it is restored. */
    int io2np = read_input(PIN_I2S_DOUT, false);
    int io2pu = read_input(PIN_I2S_DOUT, true);
    int io2pd = -1;
    gpio_config_t io2_cfg = {
        .pin_bit_mask = 1ULL << PIN_I2S_DOUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io2_cfg) == ESP_OK) io2pd = gpio_get_level(PIN_I2S_DOUT);
    release_to_default(PIN_I2S_DOUT);
    const gpio_num_t sdio[] = {PIN_C5_SDIO_D0, PIN_C5_SDIO_D1, PIN_C5_SDIO_D2,
                               PIN_C5_SDIO_D3, PIN_C5_SDIO_CLK, PIN_C5_SDIO_CMD};
    for (unsigned i = 0; i < sizeof(sdio) / sizeof(sdio[0]); ++i) release_to_default(sdio[i]);

    char line[320];
    snprintf(line, sizeof(line),
             "MEO:C5_PINS token=%s pin54=%d state=%s d0np=%d d0pu=%d "
             "clknp=%d clkpu=%d cmdnp=%d cmdpu=%d d1np=%d d2np=%d d3np=%d "
             "io2np=%d io2pu=%d io2pd=%d\n",
             token, pin54_level(), s_pin54_released ? "RELEASED" : "DEFAULT",
             d0np, d0pu, clknp, clkpu, cmdnp, cmdpu, d1np, d2np, d3np,
             io2np, io2pu, io2pd);
    (void)bsp_usb_cdc_write(line);
}

static void set_pin54(const char *token, const char *action)
{
    char line[128];
    if (strcmp(action, "HIGH") != 0) {
        /* LOW is refused on purpose: CHIP_PU would reset the C5 and SOP0 would
         * change, without a verified shared-pin sequence. */
        snprintf(line, sizeof(line),
                 "MEO:C5_PIN54 token=%s accepted=0 reason=LOW_OR_UNKNOWN_FORBIDDEN level=%d\n",
                 token, pin54_level());
        (void)bsp_usb_cdc_write(line);
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_C5_RESET,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_set_level(PIN_C5_RESET, 1);
    if (err != ESP_OK) {
        snprintf(line, sizeof(line), "MEO:C5_PIN54 token=%s accepted=0 reason=GPIO_ERROR code=0x%x\n",
                 token, (unsigned)err);
        (void)bsp_usb_cdc_write(line);
        return;
    }
    s_pin54_released = true;
    ESP_LOGW(TAG, "GPIO54 driven HIGH (C5 CHIP_PU released, radar SOP0 high)");
    snprintf(line, sizeof(line), "MEO:C5_PIN54 token=%s accepted=1 state=HIGH level=%d\n",
             token, gpio_get_level(PIN_C5_RESET));
    (void)bsp_usb_cdc_write(line);
}

bool bsp_c5_diag_command(const char *line)
{
    if (!line || strncmp(line, "WIFI ", 5) != 0) return false;

    char token[17], action[12], extra;
    if (sscanf(line, "WIFI PIN54 %16s %11s %c", token, action, &extra) == 2) {
        if (valid_token(token)) set_pin54(token, action);
        return true;
    }
    if (sscanf(line, "WIFI PINS %16s %c", token, &extra) == 1) {
        if (valid_token(token)) report_pins(token);
        return true;
    }
    /* Anything else starting with "WIFI " belongs to the SDIO probe handler. */
    return false;
}
