#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fls2_display_startup.h"
#include "driver/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/mipi_dsi_ll.h"
#include "freertos/task.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

typedef enum {
    VALIDATE_GPIO, GPIO_LOW, GPIO_CONFIG, RCC_ENTER, BUS_CLOCK_ON,
    DPI_CLOCK_OFF, RCC_EXIT, GET_HOST, HOST_OFF, PHY_OFF, DELAY
} event_t;

static const char *const event_names[] = {
    "validate GPIO", "GPIO low", "GPIO output config", "RCC enter",
    "bus clock on", "DPI clock off", "RCC exit", "get host",
    "host off", "PHY off", "delay"
};

static struct {
    dsi_host_dev_t host;
    dsi_host_dev_t registers_before;
    fls2_display_startup_state_t state_before;
    const fls2_display_startup_state_t *state_address;
    event_t events[32];
    size_t event_count;
    int expected_gpio;
    int denied_output_gpio;
    esp_err_t level_error;
    esp_err_t config_error;
    bool rcc_active;
    bool bus_clock;
    bool dpi_clock;
    bool stuck_host;
    bool stuck_phy;
    unsigned delays;
} mock;

_Static_assert(sizeof(mock_dsi_power_reg_t) == sizeof(uint32_t), "32-bit host mock");
_Static_assert(sizeof(mock_dsi_phy_reg_t) == sizeof(uint32_t), "32-bit PHY mock");

static void expect_state_equal(const fls2_display_startup_state_t *actual,
                               const fls2_display_startup_state_t *expected)
{
    CHECK(actual->host_before == expected->host_before);
    CHECK(actual->phy_before == expected->phy_before);
    CHECK(actual->host_after == expected->host_after);
    CHECK(actual->phy_after == expected->phy_after);
    CHECK(actual->prepared == expected->prepared);
}

static void record(event_t event)
{
    CHECK(mock.event_count < sizeof(mock.events) / sizeof(mock.events[0]));
    CHECK(fls2_display_startup_state() == mock.state_address);
    /* Includes vTaskDelay: prepared must only become true AFTER it returns. */
    CHECK(!fls2_display_startup_state()->prepared);
    mock.events[mock.event_count++] = event;
}

static void expect_trace(const event_t *expected, size_t count)
{
    if (mock.event_count != count) {
        fprintf(stderr, "event count: got %zu, expected %zu\n", mock.event_count, count);
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < count; ++i) {
        if (mock.events[i] != expected[i]) {
            fprintf(stderr, "event %zu: got %s, expected %s\n", i,
                    event_names[mock.events[i]], event_names[expected[i]]);
            exit(EXIT_FAILURE);
        }
    }
    CHECK(!mock.rcc_active);
}

#define EXPECT_TRACE(...) \
    expect_trace((const event_t[]){__VA_ARGS__}, \
                 sizeof((const event_t[]){__VA_ARGS__}) / sizeof(event_t))

static void expect_registers(const dsi_host_dev_t *expected)
{
    CHECK(mock.host.pwr_up.val == expected->pwr_up.val);
    CHECK(mock.host.phy_rstz.val == expected->phy_rstz.val);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(mock.host.unrelated_canaries[i] == expected->unrelated_canaries[i]);
    }
}

static void expect_captured_before(void)
{
    const fls2_display_startup_state_t *state = fls2_display_startup_state();
    CHECK(state->host_before == mock.registers_before.pwr_up.val);
    CHECK(state->phy_before == mock.registers_before.phy_rstz.val);
}

static void expect_captured_after(void)
{
    const fls2_display_startup_state_t *state = fls2_display_startup_state();
    expect_captured_before();
    CHECK(state->host_after == mock.host.pwr_up.val);
    CHECK(state->phy_after == mock.host.phy_rstz.val);
}

bool mock_gpio_is_valid_output(int gpio)
{
    record(VALIDATE_GPIO);
    CHECK(gpio == mock.expected_gpio);
    expect_state_equal(fls2_display_startup_state(), &mock.state_before);
    return gpio >= 0 && gpio < 64 && gpio != mock.denied_output_gpio;
}

esp_err_t gpio_set_level(int gpio, uint32_t level)
{
    record(GPIO_LOW);
    CHECK(gpio == mock.expected_gpio);
    CHECK(level == 0);
    expect_registers(&mock.registers_before);
    expect_state_equal(fls2_display_startup_state(), &mock.state_before);
    return mock.level_error;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    record(GPIO_CONFIG);
    CHECK(config != NULL);
    CHECK(mock.expected_gpio >= 0 && mock.expected_gpio < 64);
    CHECK(config->pin_bit_mask == (UINT64_C(1) << mock.expected_gpio));
    CHECK(config->mode == GPIO_MODE_OUTPUT);
    CHECK(config->pull_up_en == GPIO_PULLUP_DISABLE);
    CHECK(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    CHECK(config->intr_type == GPIO_INTR_DISABLE);
    expect_registers(&mock.registers_before);
    expect_state_equal(fls2_display_startup_state(), &mock.state_before);
    return mock.config_error;
}

void mock_rcc_enter(void)
{
    record(RCC_ENTER);
    CHECK(!mock.rcc_active);
    mock.rcc_active = true;
    expect_registers(&mock.registers_before);
}

void mock_rcc_exit(void)
{
    record(RCC_EXIT);
    CHECK(mock.rcc_active);
    CHECK(mock.bus_clock);
    CHECK(!mock.dpi_clock);
    mock.rcc_active = false;
}

void mipi_dsi_ll_enable_bus_clock(int id, bool enable)
{
    record(BUS_CLOCK_ON);
    CHECK(mock.rcc_active);
    CHECK(id == 0);
    CHECK(enable);
    mock.bus_clock = enable;
}

void mipi_dsi_ll_enable_dpi_clock(int id, bool enable)
{
    record(DPI_CLOCK_OFF);
    CHECK(mock.rcc_active);
    CHECK(id == 0);
    CHECK(!enable);
    mock.dpi_clock = enable;
}

dsi_host_dev_t *mock_mipi_dsi_get_host(int id)
{
    record(GET_HOST);
    CHECK(id == 0);
    CHECK(!mock.rcc_active);
    CHECK(mock.bus_clock && !mock.dpi_clock);
    expect_registers(&mock.registers_before);
    return &mock.host;
}

void mipi_dsi_host_ll_power_on_off(dsi_host_dev_t *host, bool on)
{
    record(HOST_OFF);
    CHECK(host == &mock.host);
    CHECK(!on);
    CHECK(!mock.rcc_active);
    expect_captured_before();
    expect_registers(&mock.registers_before);
    if (!mock.stuck_host) {
        host->pwr_up.shutdownz = on;
    }
}

void mipi_dsi_phy_ll_power_on_off(dsi_host_dev_t *host, bool on)
{
    record(PHY_OFF);
    CHECK(host == &mock.host);
    CHECK(!on);
    CHECK(!mock.rcc_active);
    expect_captured_before();
    CHECK(host->pwr_up.val == (mock.registers_before.pwr_up.val &
                             (mock.stuck_host ? UINT32_MAX : ~UINT32_C(1))));
    CHECK(host->phy_rstz.val == mock.registers_before.phy_rstz.val);
    if (!mock.stuck_phy) {
        host->phy_rstz.phy_shutdownz = on;
    }
}

void vTaskDelay(TickType_t ticks)
{
    record(DELAY);
    CHECK(!mock.rcc_active);
    CHECK(ticks == pdMS_TO_TICKS(10));
    CHECK(ticks == 5); /* The synthetic tick rate is 500 Hz. */
    CHECK(mock.host.pwr_up.shutdownz == 0);
    CHECK(mock.host.phy_rstz.phy_shutdownz == 0);
    expect_captured_after();
    ++mock.delays;
}

static void init_mock(uint32_t host, uint32_t phy)
{
    const fls2_display_startup_state_t zero = {0};
    memset(&mock, 0, sizeof(mock));
    mock.host.pwr_up.val = host;
    mock.host.phy_rstz.val = phy;
    mock.host.unrelated_canaries[0] = UINT32_C(0x12345678);
    mock.host.unrelated_canaries[1] = UINT32_C(0xa5a55a5a);
    mock.host.unrelated_canaries[2] = UINT32_C(0xdeadbeef);
    mock.host.unrelated_canaries[3] = UINT32_C(0x87654321);
    mock.denied_output_gpio = 34; /* Purely synthetic, not a silicon restriction. */
    mock.bus_clock = host != 0 || phy != 0;
    mock.dpi_clock = mock.bus_clock;
    mock.state_address = fls2_display_startup_state();
    CHECK(mock.state_address != NULL);
    CHECK(fls2_display_startup_state() == mock.state_address);
    expect_state_equal(mock.state_address, &zero);
}

static esp_err_t attempt(int gpio)
{
    mock.expected_gpio = gpio;
    mock.registers_before = mock.host;
    mock.state_before = *fls2_display_startup_state();
    mock.event_count = 0;
    mock.delays = 0;
    return fls2_display_prepare_startup(gpio);
}

static void expect_no_register_work(void)
{
    expect_registers(&mock.registers_before);
    expect_state_equal(fls2_display_startup_state(), &mock.state_before);
    CHECK(mock.delays == 0);
    CHECK(!mock.rcc_active);
}

static void expect_full_attempt(bool success)
{
    if (success) {
        EXPECT_TRACE(VALIDATE_GPIO, GPIO_LOW, GPIO_CONFIG, RCC_ENTER, BUS_CLOCK_ON,
                     DPI_CLOCK_OFF, RCC_EXIT, GET_HOST, HOST_OFF, PHY_OFF, DELAY);
    } else {
        EXPECT_TRACE(VALIDATE_GPIO, GPIO_LOW, GPIO_CONFIG, RCC_ENTER, BUS_CLOCK_ON,
                     DPI_CLOCK_OFF, RCC_EXIT, GET_HOST, HOST_OFF, PHY_OFF);
    }
    dsi_host_dev_t expected = mock.registers_before;
    if (!mock.stuck_host) {
        expected.pwr_up.val &= ~UINT32_C(1);
    }
    if (!mock.stuck_phy) {
        expected.phy_rstz.val &= ~UINT32_C(1);
    }
    expect_registers(&expected);
    expect_captured_after();
    CHECK(mock.bus_clock && !mock.dpi_clock);
    CHECK(mock.delays == (success ? 1U : 0U));
    CHECK(fls2_display_startup_state()->prepared == success);
}

static void expect_success(int gpio)
{
    CHECK(attempt(gpio) == ESP_OK);
    expect_full_attempt(true);
}

static void test_invalid(int gpio, bool retry)
{
    init_mock(UINT32_C(0x81000001), UINT32_C(0x42000007));
    const bool bus = mock.bus_clock;
    const bool dpi = mock.dpi_clock;
    CHECK(attempt(gpio) == ESP_ERR_INVALID_ARG);
    EXPECT_TRACE(VALIDATE_GPIO);
    expect_no_register_work();
    CHECK(mock.bus_clock == bus && mock.dpi_clock == dpi);
    if (retry) {
        expect_success(12);
    }
}

static void test_gpio_error(bool config, bool retry)
{
    init_mock(UINT32_C(0x81000001), UINT32_C(0x42000007));
    if (config) {
        mock.config_error = ESP_FAIL;
    } else {
        mock.level_error = ESP_ERR_TIMEOUT;
    }
    CHECK(attempt(12) == (config ? ESP_FAIL : ESP_ERR_TIMEOUT));
    if (config) {
        EXPECT_TRACE(VALIDATE_GPIO, GPIO_LOW, GPIO_CONFIG);
    } else {
        EXPECT_TRACE(VALIDATE_GPIO, GPIO_LOW);
    }
    expect_no_register_work();
    CHECK(mock.bus_clock && mock.dpi_clock);
    if (retry) {
        mock.level_error = ESP_OK;
        mock.config_error = ESP_OK;
        expect_success(13);
    }
}

static void test_readback(bool host, bool phy, bool retry, bool gpio_error_between)
{
    init_mock(UINT32_C(0x81000001), UINT32_C(0x42000007));
    mock.stuck_host = host;
    mock.stuck_phy = phy;
    CHECK(attempt(12) == ESP_ERR_INVALID_STATE);
    expect_full_attempt(false);
    if (gpio_error_between) {
        /* A later early failure must preserve the previous failure snapshot. */
        mock.config_error = ESP_ERR_TIMEOUT;
        CHECK(attempt(13) == ESP_ERR_TIMEOUT);
        EXPECT_TRACE(VALIDATE_GPIO, GPIO_LOW, GPIO_CONFIG);
        expect_no_register_work();
        mock.config_error = ESP_OK;
    }
    if (retry) {
        mock.stuck_host = false;
        mock.stuck_phy = false;
        /* Different inherited values prove that retry refreshes both snapshots. */
        mock.host.pwr_up.val = UINT32_C(0x24000001);
        mock.host.phy_rstz.val = UINT32_C(0x18000005);
        expect_success(13);
    }
}

static void test_repeated(bool invalid_gpio)
{
    init_mock(UINT32_C(0x81000001), UINT32_C(0x42000007));
    expect_success(12);
    /* Even a changed simulated peripheral must remain untouched after success. */
    mock.host.pwr_up.val = UINT32_C(0xff000001);
    mock.host.phy_rstz.val = UINT32_C(0xaa000007);
    mock.level_error = ESP_FAIL;
    mock.config_error = ESP_FAIL;
    CHECK(attempt(invalid_gpio ? INT_MIN : 13) == ESP_ERR_INVALID_STATE);
    expect_trace(NULL, 0); /* No GPIO validation, GPIO/RCC/register calls, or delay. */
    expect_no_register_work();
    CHECK(fls2_display_startup_state()->prepared);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *name = argv[1];
    if (strcmp(name, "initial") == 0) {
        init_mock(0, 0);
        expect_trace(NULL, 0);
    } else if (strcmp(name, "cold") == 0) {
        init_mock(0, 0);
        expect_success(12);
    } else if (strcmp(name, "warm") == 0) {
        init_mock(UINT32_C(0x81000001), UINT32_C(0x42000007));
        expect_success(12);
    } else if (strcmp(name, "reserved_bits") == 0) {
        init_mock(UINT32_C(0xfffffffe), UINT32_C(0xfffffffe));
        expect_success(12); /* Nonzero .val is valid when shutdown bits are zero. */
    } else if (strcmp(name, "gpio_zero") == 0) {
        init_mock(1, 7);
        expect_success(0);
    } else if (strcmp(name, "gpio_high") == 0) {
        init_mock(1, 7);
        expect_success(63); /* Exercise the highest safe 64-bit mask shift. */
    } else if (strcmp(name, "invalid_negative") == 0) {
        test_invalid(-1, false);
    } else if (strcmp(name, "invalid_int_min") == 0) {
        test_invalid(INT_MIN, false);
    } else if (strcmp(name, "invalid_shift_width") == 0) {
        test_invalid(64, false);
    } else if (strcmp(name, "invalid_int_max") == 0) {
        test_invalid(INT_MAX, false);
    } else if (strcmp(name, "invalid_output") == 0) {
        test_invalid(34, false);
    } else if (strcmp(name, "retry_invalid") == 0) {
        test_invalid(64, true);
    } else if (strcmp(name, "gpio_level_error") == 0) {
        test_gpio_error(false, false);
    } else if (strcmp(name, "gpio_config_error") == 0) {
        test_gpio_error(true, false);
    } else if (strcmp(name, "retry_gpio_level") == 0) {
        test_gpio_error(false, true);
    } else if (strcmp(name, "retry_gpio_config") == 0) {
        test_gpio_error(true, true);
    } else if (strcmp(name, "readback_host") == 0) {
        test_readback(true, false, false, false);
    } else if (strcmp(name, "readback_phy") == 0) {
        test_readback(false, true, false, false);
    } else if (strcmp(name, "readback_both") == 0) {
        test_readback(true, true, false, false);
    } else if (strcmp(name, "retry_readback_host") == 0) {
        test_readback(true, false, true, false);
    } else if (strcmp(name, "retry_readback_phy") == 0) {
        test_readback(false, true, true, false);
    } else if (strcmp(name, "retry_readback_both") == 0) {
        test_readback(true, true, true, false);
    } else if (strcmp(name, "retry_readback_then_gpio_error") == 0) {
        test_readback(true, true, true, true);
    } else if (strcmp(name, "repeated_success") == 0) {
        test_repeated(false);
    } else if (strcmp(name, "repeated_invalid_gpio") == 0) {
        test_repeated(true);
    } else {
        fprintf(stderr, "Unknown test case: %s\n", name);
        return EXIT_FAILURE;
    }
    printf("PASS %s (host mocks only)\n", name);
    return EXIT_SUCCESS;
}
