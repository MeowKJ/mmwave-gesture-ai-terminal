/* Compile ../../main/bsp_wifi_probe.c separately. Never include its .c file,
 * copy its implementation, expose statics, or reset production state.
 * Every scenario runs in a fresh process, with state observed over real USB
 * reply strings and dependency calls. CHECK remains active under NDEBUG. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "bsp_wifi_probe.h"
#include "bsp_usb.h"
#include "driver/gpio.h"
#include "driver/sd_host.h"
#include "driver/sd_host_sdmmc.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s (scenario=%s events=%s reply=%s)\n", \
            __FILE__, __LINE__, #expr, scenario, m.events, m.last_reply); \
    exit(1); } } while (0)

struct sd_host_driver_t { unsigned identity; };
struct sd_slot_driver_t { unsigned identity; };
static struct sd_host_driver_t controller = {0x61};
static struct sd_slot_driver_t slot = {0x62};
static const char *scenario = "startup";
static jmp_buf worker_exit;
static const uint32_t good_r4 = UINT32_C(0x90ff8000);

typedef enum { CTX_MAIN, CTX_USB, CTX_SERVICE, CTX_WORKER } context_t;
static struct {
    context_t context;
    unsigned lock_depth, lock_enters, lock_exits;
    portMUX_TYPE *lock;
    unsigned usb_calls, tasks, task_deletes, creates, adds, trans, removes, dels, gpios;
    unsigned timer_calls;
    bool controller_live, slot_live, hidden_live;
    bool hidden_on_create_error, eager_worker, worker_active;
    bool hook_fired;
    char hook_at;
    const char *probe_token;
    BaseType_t task_result;
    esp_err_t create_error, add_error, trans_error, cmd_error;
    esp_err_t remove_error, del_error, gpio_error, usb_error;
    unsigned gpio_error_index;
    uint32_t r4;
    int64_t now_us, transaction_us;
    TaskFunction_t pending;
    void *pending_arg;
    char events[128], last_reply[512];
    size_t event_len;
} m;

static void service(void);
static void run_worker(void);
static void running_hook(char point);

static void event(char code)
{
    CHECK(m.event_len + 1 < sizeof(m.events));
    m.events[m.event_len++] = code;
    m.events[m.event_len] = '\0';
}

void wifi_test_enter_critical(portMUX_TYPE *mux)
{
    CHECK(mux != NULL && mux->held == 0 && m.lock_depth == 0);
    if (!m.lock) m.lock = mux;
    CHECK(m.lock == mux);
    mux->held = 1;
    ++m.lock_depth;
    ++m.lock_enters;
}

void wifi_test_exit_critical(portMUX_TYPE *mux)
{
    CHECK(mux == m.lock && mux->held == 1 && m.lock_depth == 1);
    mux->held = 0;
    --m.lock_depth;
    ++m.lock_exits;
}

static bool command(const char *line)
{
    context_t saved = m.context;
    m.context = CTX_USB;
    unsigned tasks = m.tasks, creates = m.creates, adds = m.adds, trans = m.trans;
    unsigned removes = m.removes, dels = m.dels, gpios = m.gpios, timers = m.timer_calls;
    bool claimed = bsp_wifi_probe_command(line);
    CHECK(m.lock_depth == 0 && m.lock_enters == m.lock_exits);
    /* No task creation, hardware access, time polling or cleanup in callback,
     * including the callbacks injected at every worker/cleanup checkpoint. */
    CHECK(m.tasks == tasks && m.creates == creates && m.adds == adds);
    CHECK(m.trans == trans && m.removes == removes && m.dels == dels);
    CHECK(m.gpios == gpios && m.timer_calls == timers);
    m.context = saved;
    return claimed;
}

esp_err_t bsp_usb_cdc_write(const char *text)
{
    CHECK(m.lock_depth == 0);
    CHECK(m.context == CTX_USB); /* worker never calls USB or waits for USB */
    CHECK(text != NULL && strlen(text) < sizeof(m.last_reply));
    CHECK(strchr(text, '\n') == text + strlen(text) - 1);
    memcpy(m.last_reply, text, strlen(text) + 1);
    ++m.usb_calls;
    return m.usb_error;
}

static void expect_status(const char *request, const char *probe,
                          const char *state, const char *result,
                          esp_err_t error, uint32_t ocr, uint32_t elapsed,
                          esp_err_t cleanup, bool blocked)
{
    char input[64], expected[512];
    snprintf(input, sizeof(input), "WIFI STATUS %s", request);
    unsigned before = m.usb_calls;
    CHECK(command(input));
    CHECK(m.usb_calls == before + 1);
    snprintf(expected, sizeof(expected),
             "MEO:WIFI token=%s probe=%s state=%s result=%s error=0x%x "
             "ocr=0x%08lx funcs=%u ready=%u elapsed_ms=%lu cleanup=0x%x "
             "blocked=%u hosted=NOT_VERIFIED wifi=UNAVAILABLE reset=0\n",
             request, probe, state, result, (unsigned)error, (unsigned long)ocr,
             (unsigned)((ocr >> 28) & 7), (unsigned)((ocr >> 31) & 1),
             (unsigned long)elapsed, (unsigned)cleanup, (unsigned)blocked);
    CHECK(strcmp(m.last_reply, expected) == 0);
}

static void request_probe(const char *token, bool accepted)
{
    char input[64], expected[128];
    snprintf(input, sizeof(input), "WIFI PROBE %s", token);
    snprintf(expected, sizeof(expected), "MEO:WIFI_ACCEPT token=%s accepted=%u\n",
             token, (unsigned)accepted);
    unsigned before = m.usb_calls;
    CHECK(command(input));
    CHECK(m.usb_calls == before + 1);
    CHECK(strcmp(m.last_reply, expected) == 0);
    if (accepted) m.probe_token = token;
}

static void service(void)
{
    context_t saved = m.context;
    m.context = CTX_SERVICE;
    bsp_wifi_probe_service();
    CHECK(m.lock_depth == 0 && m.lock_enters == m.lock_exits);
    m.context = saved;
}

static void running_hook(char point)
{
    if (m.hook_at != point || m.hook_fired) return;
    m.hook_fired = true;
    unsigned tasks = m.tasks, creates = m.creates;
    request_probe("Intruder9", false);
    expect_status("Mid9", m.probe_token, "RUNNING", "PENDING", 0, 0, 0, 0, false);
    service();
    CHECK(m.tasks == tasks && m.creates == creates);
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *ret_handle)
{
    CHECK(m.context == CTX_SERVICE && m.lock_depth == 0);
    CHECK(task != NULL && strcmp(name, "wifi_probe") == 0);
    CHECK(stack_depth == 4096 && arg == NULL && priority == 1 && ret_handle == NULL);
    CHECK(m.pending == NULL && !m.worker_active);
    event('T');
    ++m.tasks;
    running_hook('T');
    if (m.task_result != pdPASS) return m.task_result;
    m.pending = task;
    m.pending_arg = arg;
    if (m.eager_worker) run_worker(); /* worker may finish before xTaskCreate returns */
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    CHECK(m.context == CTX_WORKER && m.worker_active && m.lock_depth == 0);
    CHECK(task == NULL);
    event('V');
    ++m.task_deletes;
    longjmp(worker_exit, 1); /* self-delete must not return */
}

static void worker_call(void)
{
    CHECK(m.context == CTX_WORKER && m.worker_active && m.lock_depth == 0);
}

int64_t esp_timer_get_time(void)
{
    worker_call();
    ++m.timer_calls;
    return m.now_us;
}

esp_err_t sd_host_create_sdmmc_controller(const sd_host_sdmmc_cfg_t *cfg,
                                         sd_host_ctlr_handle_t *out)
{
    worker_call();
    CHECK(cfg != NULL && cfg->event_queue_items == 4 && cfg->dma_desc_num == 4);
    CHECK(out != NULL && *out == NULL);
    CHECK(!m.controller_live && !m.slot_live && !m.hidden_live);
    event('C');
    ++m.creates;
    m.now_us += 1000;
    running_hook('C');
    if (m.create_error != ESP_OK) {
        /* v6.1 DMA-allocation/reset failure can keep a private allocation
         * without returning a handle. The app cannot safely free this. */
        m.hidden_live = m.hidden_on_create_error;
        return m.create_error;
    }
    m.controller_live = true;
    *out = &controller;
    return ESP_OK;
}

esp_err_t sd_host_sdmmc_controller_add_slot(sd_host_ctlr_handle_t ctlr,
                                           const sd_host_slot_sdmmc_init_cfg_t *cfg,
                                           sd_host_slot_handle_t *out)
{
    worker_call();
    CHECK(ctlr == &controller && m.controller_live && !m.slot_live);
    CHECK(cfg != NULL && out != NULL && *out == NULL);
    CHECK(cfg->slot_id == 1 && cfg->sd_mode == SD_MODE_NORMAL);
    CHECK(cfg->io_config.width == SD_BUS_WIDTH_1_BIT);
    CHECK(cfg->io_config.d0_io == 14 && cfg->io_config.clk_io == 18 && cfg->io_config.cmd_io == 19);
    const gpio_num_t unused[] = {cfg->io_config.d1_io, cfg->io_config.d2_io,
        cfg->io_config.d3_io, cfg->io_config.d4_io, cfg->io_config.d5_io,
        cfg->io_config.d6_io, cfg->io_config.d7_io, cfg->io_config.cd_io, cfg->io_config.wp_io};
    for (size_t i = 0; i < sizeof(unused) / sizeof(unused[0]); ++i) CHECK(unused[i] == GPIO_NUM_NC);
    CHECK(cfg->slot_flags.internal_pullup == 1 && cfg->slot_flags.wp_active_high == 0);
    event('A');
    ++m.adds;
    m.now_us += 2000;
    running_hook('A');
    if (m.add_error != ESP_OK) return m.add_error;
    m.slot_live = true;
    *out = &slot;
    return ESP_OK;
}

esp_err_t sd_host_slot_do_transaction(sd_host_slot_handle_t handle, sdmmc_command_t *cmd)
{
    worker_call();
    CHECK(handle == &slot && m.controller_live && m.slot_live);
    CHECK(cmd != NULL);
    /* Exactly one query: numeric constants intentionally independent of stubs. */
    CHECK(cmd->opcode == 5 && cmd->arg == 0 && cmd->timeout_ms == 250);
    CHECK(cmd->flags == (0x0030 | 0x1000));
    CHECK(cmd->data == NULL && cmd->datalen == 0 && cmd->buflen == 0 && cmd->blklen == 0);
    CHECK(cmd->error == ESP_OK && cmd->volt_switch_cb == NULL && cmd->volt_switch_cb_arg == NULL);
    for (unsigned i = 0; i < 4; ++i) CHECK(cmd->response[i] == 0);
    CHECK(m.trans < m.tasks); /* no retries within a worker */
    event('X');
    ++m.trans;
    m.now_us += m.transaction_us;
    running_hook('X');
    cmd->response[0] = m.r4;
    cmd->response[1] = UINT32_MAX; /* only R4's first response word matters */
    cmd->error = m.cmd_error;
    return m.trans_error;
}

esp_err_t sd_host_remove_slot(sd_host_slot_handle_t handle)
{
    worker_call();
    CHECK(handle == &slot && m.controller_live && m.slot_live);
    event('R');
    ++m.removes;
    m.now_us += 3000;
    running_hook('R');
    if (m.remove_error == ESP_OK) m.slot_live = false;
    return m.remove_error;
}

esp_err_t sd_host_del_controller(sd_host_ctlr_handle_t ctlr)
{
    worker_call();
    CHECK(ctlr == &controller && m.controller_live && !m.slot_live);
    event('D');
    ++m.dels;
    m.now_us += 4000;
    running_hook('D');
    if (m.del_error == ESP_OK) m.controller_live = false;
    return m.del_error;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    worker_call();
    CHECK(!m.controller_live && !m.slot_live && !m.hidden_live);
    const int allowed[] = {14, 18, 19};
    unsigned index = m.gpios % 3;
    CHECK((int)pin == allowed[index]); /* excludes ALL other pins, including 54/6/29, LCD/touch */
    event((char)('1' + index));
    ++m.gpios;
    m.now_us += 500;
    running_hook((char)('1' + index));
    return m.gpio_error_index == index + 1 ? m.gpio_error : ESP_OK;
}

static void run_worker(void)
{
    CHECK(m.pending != NULL && !m.worker_active);
    context_t saved = m.context;
    m.context = CTX_WORKER;
    m.worker_active = true;
    if (setjmp(worker_exit) == 0) {
        m.pending(m.pending_arg);
        CHECK(false); /* self-deleting task returned */
    }
    m.pending = NULL;
    m.worker_active = false;
    m.context = saved;
    CHECK(m.lock_depth == 0 && m.lock_enters == m.lock_exits);
}

static void defaults(void)
{
    /* Only dependency injection is reset. Production statics are untouched. */
    m.task_result = pdPASS;
    m.create_error = m.add_error = m.trans_error = m.cmd_error = ESP_OK;
    m.remove_error = m.del_error = m.gpio_error = m.usb_error = ESP_OK;
    m.hidden_on_create_error = m.eager_worker = false;
    m.gpio_error_index = 0;
    m.r4 = good_r4;
    m.transaction_us = 250000;
    m.hook_at = 0;
    m.hook_fired = false;
}

static void expect_idle(void)
{
    expect_status("Boot9", "none", "IDLE", "NOT_RUN", 0, 0, 0, 0, false);
    CHECK(m.tasks == 0 && m.creates == 0 && m.adds == 0 && m.trans == 0);
    CHECK(m.gpios == 0 && m.timer_calls == 0 && m.event_len == 0);
}

static void queue_and_start(void)
{
    request_probe("First9", true);
    expect_status("Queued9", "First9", "QUEUED", "PENDING", 0, 0, 0, 0, false);
    CHECK(m.tasks == 0 && m.creates == 0 && m.event_len == 0);
    request_probe("Duplicate9", false);
    expect_status("Unchanged9", "First9", "QUEUED", "PENDING", 0, 0, 0, 0, false);
    service();
    CHECK(m.tasks == 1);
}

static void no_automatic_retry(void)
{
    unsigned tasks = m.tasks, calls = m.usb_calls;
    size_t events = m.event_len;
    for (unsigned i = 0; i < 8; ++i) service();
    CHECK(m.tasks == tasks && m.event_len == events && m.usb_calls == calls);
}

static void explicit_retry(bool blocked)
{
    no_automatic_retry();
    if (blocked) {
        unsigned tasks = m.tasks;
        size_t events = m.event_len;
        for (unsigned i = 0; i < 3; ++i) {
            request_probe("Blocked9", false);
            service();
        }
        CHECK(m.tasks == tasks && m.event_len == events && m.pending == NULL);
        return;
    }
    CHECK(!m.controller_live && !m.slot_live && !m.hidden_live);
    defaults();
    unsigned tasks = m.tasks, deletes = m.task_deletes, trans = m.trans;
    size_t events = m.event_len;
    request_probe("Retry9", true);
    expect_status("Next9", "Retry9", "QUEUED", "PENDING", 0, 0, 0, 0, false);
    service();
    CHECK(m.tasks == tasks + 1 && m.pending != NULL);
    int64_t start = m.now_us;
    run_worker();
    CHECK(strcmp(m.events + events, "TCAXRD123V") == 0);
    CHECK(m.task_deletes == deletes + 1 && m.trans == trans + 1);
    CHECK(!m.controller_live && !m.slot_live && !m.hidden_live);
    expect_status("After9", "Retry9", "DONE", "SDIO_R4", ESP_OK, good_r4,
                  (uint32_t)((m.now_us - start) / 1000), ESP_OK, false);
    no_automatic_retry();
}

typedef struct { const char *name; const char *line; bool claimed; } input_case_t;
static const input_case_t invalid_inputs[] = {
    {"null", NULL, false}, {"empty", "", false},
    {"bare", "WIFI", false}, {"tab_prefix", "WIFI\tPROBE A", false},
    {"leading_space", " WIFI PROBE A", false}, {"wrong_prefix", "WIFIX PROBE A", false},
    {"lower_prefix", "wifi PROBE A", false}, {"missing_command", "WIFI ", true},
    {"missing_token", "WIFI PROBE", true}, {"blank_token", "WIFI PROBE \r\n", true},
    {"long_token", "WIFI PROBE 12345678901234567", true},
    {"huge_token", "WIFI PROBE 1234567890123456789012345678901234567890", true},
    {"underscore", "WIFI PROBE A_B", true}, {"hyphen", "WIFI PROBE A-B", true},
    {"punctuation", "WIFI PROBE A!", true}, {"equals", "WIFI PROBE token=A", true},
    {"slash", "WIFI PROBE A/B", true}, {"dot", "WIFI PROBE A.B", true},
    {"nonascii", "WIFI PROBE \xc3\xa9", true},
    {"control", "WIFI PROBE A\x01", true},
    {"extra", "WIFI PROBE A B", true}, {"extra_line", "WIFI PROBE A\nX", true},
    {"split_token", "WIFI PROBE A\tB", true},
    {"lower_command", "WIFI probe A", true}, {"unknown", "WIFI RESET A", true},
    {"long_command", "WIFI PROBEPROBEPROBEPROBE A", true},
    {"status_missing", "WIFI STATUS", true}, {"status_long", "WIFI STATUS 12345678901234567", true},
    {"status_extra", "WIFI STATUS A extra", true}, {"status_bad", "WIFI STATUS A_B", true}
};

static bool test_input(const char *name)
{
    for (size_t i = 0; i < sizeof(invalid_inputs) / sizeof(invalid_inputs[0]); ++i) {
        const input_case_t *c = &invalid_inputs[i];
        if (strcmp(name, c->name) != 0) continue;
        unsigned writes = m.usb_calls;
        CHECK(command(c->line) == c->claimed);
        CHECK(m.usb_calls == writes);
        service();
        expect_idle();
        /* A rejected input must leave parser/state/lock usable. */
        queue_and_start();
        run_worker();
        explicit_retry(false);
        return true;
    }
    return false;
}

static bool test_valid_input(const char *name)
{
    const char *input = NULL, *token = NULL;
    if (strcmp(name, "token_min") == 0) { input = "WIFI PROBE A"; token = "A"; }
    if (strcmp(name, "token_max") == 0) { input = "WIFI PROBE Az09bY18cX27dW36"; token = "Az09bY18cX27dW36"; }
    if (strcmp(name, "token_numeric") == 0) { input = "WIFI PROBE 0123456789"; token = "0123456789"; }
    if (strcmp(name, "whitespace") == 0) { input = "WIFI  PROBE\tAz09 \r\n\t"; token = "Az09"; }
    if (!input) return false;
    CHECK(command(input));
    m.probe_token = token;
    char expected[96];
    snprintf(expected, sizeof(expected), "MEO:WIFI_ACCEPT token=%s accepted=1\n", token);
    CHECK(strcmp(m.last_reply, expected) == 0);
    expect_status("S", token, "QUEUED", "PENDING", 0, 0, 0, 0, false);
    service();
    run_worker();
    expect_status("Az09bY18cX27dW36", token, "DONE", "SDIO_R4", 0, good_r4, 261, 0, false);
    explicit_retry(false);
    return true;
}

static bool test_scenario(const char *name)
{
    const char *result = "SDIO_R4", *events = "TCAXRD123V";
    esp_err_t error = ESP_OK, cleanup = ESP_OK;
    uint32_t ocr = good_r4;
    bool blocked = false;
    bool recognized = true;
    if (strcmp(name, "success") == 0) { /* defaults */ }
    else if (strcmp(name, "not_ready_r4") == 0) m.r4 = ocr = UINT32_C(0x10ff8000);
    else if (strcmp(name, "seven_funcs_r4") == 0) m.r4 = ocr = UINT32_C(0xf0ff8000);
    else if (strcmp(name, "one_voltage_r4") == 0) m.r4 = ocr = UINT32_C(0x10008000);
    else if (strcmp(name, "slow_driver") == 0) m.transaction_us = 900000;
    else if (strcmp(name, "usb_failure") == 0) m.usb_error = ESP_ERR_NOT_FINISHED;
    else if (strcmp(name, "eager_worker") == 0) m.eager_worker = true;
    else if (strncmp(name, "hook_", 5) == 0 && strlen(name) == 6 && strchr("TCAXRD123", name[5])) m.hook_at = name[5];
    else if (strcmp(name, "task_failure") == 0) {
        m.task_result = pdFAIL; result = "TASK_ERROR"; error = ESP_ERR_NO_MEM;
        ocr = 0; events = "T";
    } else if (strncmp(name, "create_", 7) == 0) {
        const char *kind = name + 7;
        if (strcmp(kind, "busy") == 0) error = ESP_ERR_NOT_FOUND;
        else if (strcmp(kind, "nomem") == 0) error = ESP_ERR_NO_MEM;
        else if (strcmp(kind, "invalidarg") == 0) error = ESP_ERR_INVALID_ARG;
        else if (strcmp(kind, "timeout") == 0) error = ESP_ERR_TIMEOUT;
        else if (strcmp(kind, "generic") == 0) error = ESP_FAIL;
        else if (strcmp(kind, "hidden_nomem") == 0) { error = ESP_ERR_NO_MEM; m.hidden_on_create_error = true; }
        else if (strcmp(kind, "hidden_timeout") == 0) { error = ESP_ERR_TIMEOUT; m.hidden_on_create_error = true; }
        else return false;
        m.create_error = error;
        result = error == ESP_ERR_NOT_FOUND ? "HOST_BUSY" : "HOST_ERROR";
        blocked = true; ocr = 0; events = "TCV";
    } else if (strncmp(name, "add_", 4) == 0) {
        const char *kind = name + 4;
        if (strcmp(kind, "nomem") == 0) error = ESP_ERR_NO_MEM;
        else if (strcmp(kind, "invalidstate") == 0) error = ESP_ERR_INVALID_STATE;
        else if (strcmp(kind, "invalidarg") == 0) error = ESP_ERR_INVALID_ARG;
        else if (strcmp(kind, "generic") == 0) error = ESP_FAIL;
        else if (strcmp(kind, "delete_failure") == 0) {
            error = ESP_ERR_NO_MEM; cleanup = m.del_error = ESP_ERR_INVALID_STATE;
        } else if (strcmp(kind, "gpio_failure") == 0) {
            error = ESP_ERR_INVALID_ARG; cleanup = m.gpio_error = ESP_FAIL; m.gpio_error_index = 1;
        } else return false;
        m.add_error = error; result = "HOST_ERROR"; ocr = 0;
        blocked = cleanup != ESP_OK;
        events = m.del_error == ESP_OK ? "TCAD123V" : "TCADV";
    } else if (strncmp(name, "trans_", 6) == 0 || strncmp(name, "cmd_", 4) == 0) {
        bool cmd = name[0] == 'c';
        const char *kind = name + (cmd ? 4 : 6);
        if (strcmp(kind, "timeout") == 0) error = ESP_ERR_TIMEOUT;
        else if (strcmp(kind, "crc") == 0) error = ESP_ERR_INVALID_CRC;
        else if (strcmp(kind, "invalidarg") == 0) error = ESP_ERR_INVALID_ARG;
        else if (strcmp(kind, "generic") == 0) error = ESP_FAIL;
        else if (strcmp(kind, "precedence") == 0 && !cmd) { error = ESP_ERR_INVALID_CRC; m.cmd_error = ESP_ERR_TIMEOUT; }
        else if (strcmp(kind, "timeout_precedence") == 0 && !cmd) { error = ESP_ERR_TIMEOUT; m.cmd_error = ESP_FAIL; }
        else return false;
        if (cmd) m.cmd_error = error; else m.trans_error = error;
        result = error == ESP_ERR_TIMEOUT ? "NO_RESPONSE" : "TRANSPORT_ERROR";
        ocr = 0;
    } else if (strncmp(name, "r4_", 3) == 0) {
        const char *kind = name + 3;
        if (strcmp(kind, "zero") == 0) m.r4 = 0;
        else if (strcmp(kind, "ones") == 0) m.r4 = UINT32_MAX;
        else if (strcmp(kind, "no_funcs") == 0) m.r4 = UINT32_C(0x80ff8000);
        else if (strcmp(kind, "no_voltage") == 0) m.r4 = UINT32_C(0x90000000);
        else if (strcmp(kind, "ready_only") == 0) m.r4 = UINT32_C(0x80000000);
        else if (strcmp(kind, "funcs_only") == 0) m.r4 = UINT32_C(0x10000000);
        else return false;
        ocr = m.r4; error = ESP_ERR_INVALID_RESPONSE; result = "INVALID_R4";
    } else if (strncmp(name, "cleanup_", 8) == 0) {
        const char *kind = name + 8;
        cleanup = ESP_FAIL; blocked = true;
        if (strcmp(kind, "remove") == 0) { m.remove_error = cleanup; events = "TCAXRV"; }
        else if (strcmp(kind, "delete") == 0) { m.del_error = cleanup; events = "TCAXRDV"; }
        else if (strcmp(kind, "gpio14") == 0) { m.gpio_error = cleanup; m.gpio_error_index = 1; }
        else if (strcmp(kind, "gpio18") == 0) { m.gpio_error = cleanup; m.gpio_error_index = 2; }
        else if (strcmp(kind, "gpio19") == 0) { m.gpio_error = cleanup; m.gpio_error_index = 3; }
        else if (strcmp(kind, "timeout_remove") == 0) {
            m.trans_error = error = ESP_ERR_TIMEOUT; result = "NO_RESPONSE"; ocr = 0;
            m.remove_error = cleanup; events = "TCAXRV";
        } else if (strcmp(kind, "cmd_delete") == 0) {
            m.cmd_error = error = ESP_ERR_INVALID_CRC; result = "TRANSPORT_ERROR"; ocr = 0;
            m.del_error = cleanup; events = "TCAXRDV";
        } else if (strcmp(kind, "r4_gpio") == 0) {
            m.r4 = ocr = 0; error = ESP_ERR_INVALID_RESPONSE; result = "INVALID_R4";
            m.gpio_error = cleanup; m.gpio_error_index = 2;
        } else if (strcmp(kind, "first_error") == 0) {
            m.remove_error = cleanup = ESP_ERR_INVALID_STATE;
            m.del_error = ESP_ERR_NO_MEM; m.gpio_error = ESP_FAIL; m.gpio_error_index = 1;
            events = "TCAXRV";
        } else return false;
    } else recognized = false;
    if (!recognized) return false;

    int64_t start = m.now_us;
    queue_and_start();
    if (m.task_result == pdPASS && !m.eager_worker) {
        expect_status("Before9", "First9", "RUNNING", "PENDING", 0, 0, 0, 0, false);
        request_probe("Running9", false);
        for (unsigned i = 0; i < 4; ++i) service();
        CHECK(m.tasks == 1 && m.creates == 0);
        run_worker();
    }
    CHECK(strcmp(m.events, events) == 0);
    CHECK(m.task_deletes == (m.task_result == pdPASS ? 1U : 0U));
    CHECK(m.timer_calls == (m.task_result == pdPASS ? 2U : 0U));
    if (m.hook_at) CHECK(m.hook_fired);
    CHECK(m.slot_live == (m.remove_error != ESP_OK));
    CHECK(m.controller_live == (m.remove_error != ESP_OK || m.del_error != ESP_OK));
    CHECK(m.hidden_live == m.hidden_on_create_error);
    expect_status("Done9", "First9", "DONE", result, error, ocr,
                  (uint32_t)((m.now_us - start) / 1000), cleanup, blocked);
    unsigned writes = m.usb_calls;
    explicit_retry(blocked);
    if (blocked) {
        CHECK(m.usb_calls == writes + 3);
        expect_status("Still9", "First9", "DONE", result, error, ocr,
                      (uint32_t)((m.now_us - start) / 1000), cleanup, true);
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "Expected one registered scenario name\n"); return 2; }
    scenario = argv[1];
    defaults();
    m.now_us = 1000000;
    /* Calling service at boot and requesting STATUS must never touch hardware. */
    for (unsigned i = 0; i < 8; ++i) service();
    expect_idle();
    bool handled = false;
    if (strcmp(scenario, "startup") == 0) {
        expect_idle();
        handled = true;
    } else if (strncmp(scenario, "input_", 6) == 0) handled = test_input(scenario + 6);
    else if (test_valid_input(scenario)) handled = true;
    else handled = test_scenario(scenario);
    if (!handled) { fprintf(stderr, "Unknown scenario: %s\n", scenario); return 2; }
    CHECK(m.lock_depth == 0 && m.lock_enters == m.lock_exits);
    CHECK(m.pending == NULL && !m.worker_active);
    printf("PASS %s (tasks=%u CMD5=%u events=%s)\n", scenario, m.tasks, m.trans, m.events);
    return 0;
}
