#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_usb_tx.h"
#include "sdkconfig.h"
#include "freertos/portmacro.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

static const char *s_case;

/* Unlike assert(), these checks remain active in Release / with NDEBUG. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", \
                __FILE__, __LINE__, s_case, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum { MAX_CALLS = 32, MAX_WIRE = 4096 };

typedef struct {
    size_t requested;
    size_t accepted;
    uint8_t bytes[CONFIG_TINYUSB_CDC_TX_BUFSIZE];
} write_call_t;

typedef struct {
    bool mounted;
    bool isr;
    uint32_t capacity;
    esp_err_t flush_result;
    size_t returns[MAX_CALLS];
    size_t return_count;
    size_t return_index;
    unsigned isr_calls;
    unsigned mounted_calls;
    unsigned available_calls;
    unsigned write_calls;
    unsigned flush_calls;
    write_call_t writes[MAX_CALLS];
    uint8_t wire[MAX_WIRE];
    size_t wire_len;
    char trace[MAX_CALLS * 3 + 1];
    size_t trace_len;
    char reenter_event;
    unsigned reenter_call;
    char followup_event;
    unsigned followup_call;
    unsigned reenter_count;
    esp_err_t reenter_result;
} mock_t;

static mock_t m;

static void reset_mock(void)
{
    /* Only dependencies reset. The production statics remain untouched. */
    memset(&m, 0, sizeof(m));
    m.mounted = true;
    m.capacity = CONFIG_TINYUSB_CDC_TX_BUFSIZE;
    m.flush_result = ESP_OK;
}

static void event(char kind, unsigned call)
{
    CHECK(m.trace_len + 1 < sizeof(m.trace));
    m.trace[m.trace_len++] = kind;
    m.trace[m.trace_len] = '\0';
    if (m.reenter_event == kind && m.reenter_call == call) {
        unsigned available_before = m.available_calls;
        unsigned write_before = m.write_calls;
        unsigned flush_before = m.flush_calls;
        /* One deterministic competing producer, while the outer call owns
         * the real production trylock. Disable the hook before reentering. */
        m.reenter_event = '\0';
        ++m.reenter_count;
        m.reenter_result = bsp_usb_tx_record("rival\n", 6, true);
        CHECK(m.reenter_result == ESP_ERR_NOT_FINISHED);
        CHECK(m.available_calls == available_before);
        CHECK(m.write_calls == write_before);
        CHECK(m.flush_calls == flush_before);
        m.reenter_event = m.followup_event;
        m.reenter_call = m.followup_call;
        m.followup_event = '\0';
    }
}

int xPortInIsrContext(void)
{
    ++m.isr_calls;
    return m.isr ? 1 : 0;
}

bool tud_mounted(void)
{
    ++m.mounted_calls;
    return m.mounted;
}

uint32_t tud_cdc_n_write_available(uint8_t itf)
{
    CHECK(itf == TINYUSB_CDC_ACM_0);
    event('A', ++m.available_calls);
    return m.capacity;
}

size_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf,
                                const uint8_t *in_buf, size_t in_size)
{
    CHECK(itf == TINYUSB_CDC_ACM_0);
    CHECK(in_buf != NULL);
    CHECK(in_size > 0 && in_size <= CONFIG_TINYUSB_CDC_TX_BUFSIZE);
    CHECK(m.write_calls < MAX_CALLS);
    write_call_t *call = &m.writes[m.write_calls];
    call->requested = in_size;
    memcpy(call->bytes, in_buf, in_size);
    event('W', ++m.write_calls);
    size_t accepted = in_size;
    if (m.return_index < m.return_count) {
        accepted = m.returns[m.return_index++];
    }
    CHECK(accepted <= in_size);
    CHECK(accepted <= m.capacity);
    CHECK(m.wire_len + accepted <= sizeof(m.wire));
    call->accepted = accepted;
    memcpy(m.wire + m.wire_len, in_buf, accepted);
    m.wire_len += accepted;
    m.capacity -= (uint32_t)accepted;
    return accepted;
}

esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf,
                                  uint32_t timeout_ticks)
{
    CHECK(itf == TINYUSB_CDC_ACM_0);
    /* Checked on EVERY flush, including rejection, recovery and reentry. */
    CHECK(timeout_ticks == 0);
    event('F', ++m.flush_calls);
    return m.flush_result;
}

#if CONFIG_APP_USB_HID_ENABLE

static void expect_trace(const char *trace)
{
    CHECK(strcmp(m.trace, trace) == 0);
    CHECK(m.return_index == m.return_count);
}

static void expect_wire(const char *bytes, size_t len)
{
    CHECK(m.wire_len == len);
    CHECK(memcmp(m.wire, bytes, len) == 0);
}

static void expect_write(unsigned index, const char *bytes, size_t len,
                         size_t accepted)
{
    CHECK(index < m.write_calls);
    CHECK(m.writes[index].requested == len);
    CHECK(m.writes[index].accepted == accepted);
    CHECK(memcmp(m.writes[index].bytes, bytes, len) == 0);
}

/* Keep the received stream and production boundary flag; clear only dependency
 * observations/injections between two calls of the SAME scenario. */
static void next_call(uint32_t capacity)
{
    uint8_t saved[MAX_WIRE];
    size_t saved_len = m.wire_len;
    memcpy(saved, m.wire, saved_len);
    reset_mock();
    memcpy(m.wire, saved, saved_len);
    m.wire_len = saved_len;
    m.capacity = capacity;
}

static void force_write(size_t accepted)
{
    CHECK(m.return_count < MAX_CALLS);
    m.returns[m.return_count++] = accepted;
}

static void seed_short_write(size_t accepted)
{
    force_write(accepted);
    CHECK(bsp_usb_tx_record("broken\n", 7, true) == ESP_FAIL);
    expect_trace("AWF");
    expect_write(0, "broken\n", 7, accepted);
    expect_wire("broken\n", accepted);
}

static void expect_repair_success(void)
{
    size_t prefix = m.wire_len;
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_OK);
    expect_trace("AWAWF");
    CHECK(m.write_calls == 2);
    expect_write(0, "\n", 1, 1);
    expect_write(1, "ok\n", 3, 3);
    CHECK(m.wire_len == prefix + 4);
    CHECK(memcmp(m.wire + prefix, "\nok\n", 4) == 0);
}

static void success_byte_count(void)
{
    CHECK((size_t)ESP_OK != 3);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_OK);
    expect_trace("AWF");
    CHECK(m.write_calls == 1 && m.flush_calls == 1);
    expect_write(0, "ok\n", 3, 3);
    expect_wire("ok\n", 3);
}

static void newline_only(void)
{
    m.capacity = 1;
    CHECK(bsp_usb_tx_record("\n", 1, true) == ESP_OK);
    expect_trace("AWF");
    expect_wire("\n", 1);
    CHECK(m.capacity == 0);
}

static void byte_count_equals_error(void)
{
    /* A valid byte count can even have the SAME numerical value as an ESP
     * error constant. Its type/API contract still makes it queue success. */
    char record[ESP_ERR_NO_MEM];
    CHECK(sizeof(record) <= CONFIG_TINYUSB_CDC_TX_BUFSIZE);
    memset(record, 'b', sizeof(record));
    record[sizeof(record) - 1] = '\n';
    m.capacity = (uint32_t)sizeof(record);
    CHECK(bsp_usb_tx_record(record, sizeof(record), true) == ESP_OK);
    expect_trace("AWF");
    expect_write(0, record, sizeof(record), sizeof(record));
    expect_wire(record, sizeof(record));
    CHECK(m.capacity == 0);
}

static void exact_capacity(void)
{
    m.capacity = 3;
    success_byte_count();
    CHECK(m.capacity == 0);
}

static void maximum_record(void)
{
    char record[CONFIG_TINYUSB_CDC_TX_BUFSIZE];
    memset(record, 'x', sizeof(record));
    record[sizeof(record) - 1] = '\n';
    CHECK(bsp_usb_tx_record(record, sizeof(record), true) == ESP_OK);
    expect_trace("AWF");
    expect_write(0, record, sizeof(record), sizeof(record));
    expect_wire(record, sizeof(record));
    CHECK(m.capacity == 0);
}

static void explicit_length(void)
{
    /* No trailing C-string NUL is required or sent. */
    const char record[] = { 'o', 'k', '\n' };
    CHECK(bsp_usb_tx_record(record, sizeof(record), true) == ESP_OK);
    expect_trace("AWF");
    expect_wire(record, sizeof(record));
    expect_write(0, record, sizeof(record), sizeof(record));
}

static void capacity_rejected(uint32_t capacity)
{
    m.capacity = capacity;
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_NO_MEM);
    expect_trace("AF");
    CHECK(m.write_calls == 0 && m.flush_calls == 1);
    expect_wire("", 0);
}

static void capacity_zero(void) { capacity_rejected(0); }
static void capacity_one_short(void) { capacity_rejected(2); }

static void capacity_consumed(void)
{
    m.capacity = 5;
    success_byte_count();
    next_call(m.capacity);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_NO_MEM);
    expect_trace("AF");
    CHECK(m.write_calls == 0);
    expect_wire("ok\n", 3);
    next_call(3); /* The consumer has freed three slots. */
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_OK);
    expect_trace("AWF");
    expect_wire("ok\nok\n", 6);
}

static void invalid(const char *record, size_t len)
{
    CHECK(bsp_usb_tx_record(record, len, true) == ESP_ERR_INVALID_ARG);
    expect_trace("");
    CHECK(m.isr_calls == 0 && m.mounted_calls == 0);
    CHECK(m.write_calls == 0 && m.flush_calls == 0);
    expect_wire("", 0);
}

static void invalid_null(void) { invalid(NULL, 3); }
static void invalid_zero(void) { invalid("\n", 0); }
static void invalid_size_max(void) { invalid("\n", SIZE_MAX); }
static void invalid_missing_newline(void) { invalid("oops", 4); }
static void invalid_newline_outside_length(void) { invalid("x\n", 1); }
static void invalid_trailing_space(void) { invalid("x\n ", 3); }

static void invalid_oversize(void)
{
    char record[CONFIG_TINYUSB_CDC_TX_BUFSIZE + 1];
    memset(record, 'x', sizeof(record));
    record[sizeof(record) - 1] = '\n';
    invalid(record, sizeof(record));
}

static void state_rejected(bool session_open)
{
    CHECK(bsp_usb_tx_record("ok\n", 3, session_open) == ESP_ERR_INVALID_STATE);
    expect_trace("");
    CHECK(m.write_calls == 0 && m.flush_calls == 0);
    CHECK(m.isr_calls == 1);
}

static void unmounted(void) { m.mounted = false; state_rejected(true); }
static void closed(void) { state_rejected(false); CHECK(m.mounted_calls == 0); }
static void isr(void) { m.isr = true; state_rejected(true); CHECK(m.mounted_calls == 0); }

static void pending_boundary_preflight(void)
{
    seed_short_write(3);
    next_call(CONFIG_TINYUSB_CDC_TX_BUFSIZE);
    CHECK(bsp_usb_tx_record(NULL, 3, true) == ESP_ERR_INVALID_ARG);
    CHECK(bsp_usb_tx_record("ok\n", 3, false) == ESP_ERR_INVALID_STATE);
    m.mounted = false;
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_INVALID_STATE);
    m.mounted = true;
    m.isr = true;
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_INVALID_STATE);
    expect_trace("");
    CHECK(m.write_calls == 0 && m.flush_calls == 0);
    next_call(CONFIG_TINYUSB_CDC_TX_BUFSIZE);
    expect_repair_success();
    expect_wire("bro\nok\n", 7);
}

static void queue_zero(void)
{
    force_write(0); /* 0 == ESP_OK, but zero bytes queued means failure. */
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_FAIL);
    expect_trace("AWF");
    expect_write(0, "ok\n", 3, 0);
    expect_wire("", 0);
    next_call(3);
    success_byte_count(); /* No spurious boundary after a zero-byte write. */
}

static void short_then_repair(size_t accepted)
{
    seed_short_write(accepted);
    next_call(4); /* Exactly enough for the boundary plus the next record. */
    expect_repair_success();
    CHECK(m.capacity == 0);
}

static void short_write_repair(void) { short_then_repair(3); }
static void short_write_one(void) { short_then_repair(1); }
static void short_write_last_byte(void) { short_then_repair(6); }

static void boundary_retry(bool queue_fails)
{
    seed_short_write(3);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        next_call(queue_fails ? CONFIG_TINYUSB_CDC_TX_BUFSIZE : 0);
        if (queue_fails) force_write(0);
        CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_NO_MEM);
        expect_trace(queue_fails ? "AWF" : "AF");
        CHECK(m.flush_calls == 1);
        CHECK(m.write_calls == (queue_fails ? 1u : 0u));
        if (queue_fails) expect_write(0, "\n", 1, 0);
        expect_wire("bro", 3);
    }
    next_call(4);
    expect_repair_success();
    expect_wire("bro\nok\n", 7);
}

static void boundary_no_capacity_retry(void) { boundary_retry(false); }
static void boundary_write_failure_retry(void) { boundary_retry(true); }

static void boundary_consumes_capacity(uint32_t capacity)
{
    seed_short_write(3);
    next_call(capacity);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_NO_MEM);
    expect_trace("AWAF");
    CHECK(m.write_calls == 1);
    expect_write(0, "\n", 1, 1);
    expect_wire("bro\n", 4);
    next_call(3);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_OK);
    expect_trace("AWF"); /* Successful repair was not repeated. */
    expect_wire("bro\nok\n", 7);
}

static void boundary_only_capacity(void) { boundary_consumes_capacity(1); }
static void boundary_record_one_short(void) { boundary_consumes_capacity(3); }

static void boundary_then_zero_write(void)
{
    seed_short_write(3);
    next_call(4);
    force_write(1);
    force_write(0);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_FAIL);
    expect_trace("AWAWF");
    expect_write(0, "\n", 1, 1);
    expect_write(1, "ok\n", 3, 0);
    expect_wire("bro\n", 4);
    next_call(3);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_OK);
    expect_trace("AWF");
    expect_wire("bro\nok\n", 7);
}

static void boundary_then_short_write(void)
{
    seed_short_write(3);
    next_call(4);
    force_write(1);
    force_write(1);
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_FAIL);
    expect_trace("AWAWF");
    expect_write(0, "\n", 1, 1);
    expect_write(1, "ok\n", 3, 1);
    expect_wire("bro\no", 5);
    next_call(4);
    expect_repair_success();
    expect_wire("bro\no\nok\n", 9);
}

static void flush_not_finished(void)
{
    m.flush_result = ESP_ERR_NOT_FINISHED;
    success_byte_count();
}

static void flush_failure(void) { m.flush_result = ESP_FAIL; success_byte_count(); }
static void flush_no_capacity(void) { m.flush_result = ESP_ERR_NOT_FINISHED; capacity_zero(); }

static void flush_zero_write(void)
{
    m.flush_result = ESP_ERR_NOT_FINISHED;
    queue_zero();
}

static void reentrant(char kind, unsigned call, bool boundary, bool failure,
                      bool no_capacity)
{
    if (boundary) {
        seed_short_write(3);
        next_call(CONFIG_TINYUSB_CDC_TX_BUFSIZE);
    }
    m.reenter_event = kind;
    m.reenter_call = call;
    /* Also prove a pending non-blocking flush doesn't affect trylock release. */
    m.flush_result = ESP_ERR_NOT_FINISHED;
    if (failure) force_write(0);
    if (no_capacity) m.capacity = 0;
    esp_err_t expected = ESP_OK;
    if (failure) expected = boundary ? ESP_ERR_NO_MEM : ESP_FAIL;
    if (no_capacity) expected = ESP_ERR_NO_MEM;
    CHECK(bsp_usb_tx_record("ok\n", 3, true) == expected);
    CHECK(m.reenter_count == 1);
    CHECK(m.reenter_result == ESP_ERR_NOT_FINISHED);
    CHECK(m.flush_calls == 1);
    expect_trace(no_capacity ? "AF" : (boundary && !failure ? "AWAWF" : "AWF"));
    if (boundary && failure) {
        expect_write(0, "\n", 1, 0);
        expect_wire("bro", 3);
        next_call(4);
        expect_repair_success();
        expect_wire("bro\nok\n", 7);
    } else if (boundary) {
        expect_write(0, "\n", 1, 1);
        expect_write(1, "ok\n", 3, 3);
        expect_wire("bro\nok\n", 7);
    } else {
        expect_wire(expected == ESP_OK ? "ok\n" : "", expected == ESP_OK ? 3 : 0);
    }
}

static void reentrant_capacity(void) { reentrant('A', 1, false, false, false); }
static void reentrant_write(void) { reentrant('W', 1, false, false, false); }
static void reentrant_flush(void) { reentrant('F', 1, false, false, false); }
static void reentrant_zero_write(void) { reentrant('W', 1, false, true, false); }
static void reentrant_no_capacity(void) { reentrant('F', 1, false, false, true); }
static void reentrant_boundary_write(void) { reentrant('W', 1, true, false, false); }
static void reentrant_boundary_record(void) { reentrant('W', 2, true, false, false); }
static void reentrant_boundary_failure(void) { reentrant('W', 1, true, true, false); }

static void reentrant_keeps_owner_lock(void)
{
    m.reenter_event = 'W';
    m.reenter_call = 1;
    m.followup_event = 'F';
    m.followup_call = 1;
    success_byte_count();
    CHECK(m.reenter_count == 2);
    /* The rejected first rival must not unlock the outer producer: the
     * second rival during the same outer call must ALSO be rejected. */
    CHECK(m.reenter_result == ESP_ERR_NOT_FINISHED);
}

typedef void (*test_fn_t)(void);
typedef struct { const char *name; test_fn_t run; } test_case_t;

#define CASE(name) { #name, name }
static const test_case_t cases[] = {
    CASE(success_byte_count), CASE(byte_count_equals_error),
    CASE(newline_only), CASE(exact_capacity),
    CASE(maximum_record), CASE(explicit_length), CASE(capacity_zero),
    CASE(capacity_one_short), CASE(capacity_consumed),
    CASE(invalid_null), CASE(invalid_zero), CASE(invalid_oversize),
    CASE(invalid_size_max), CASE(invalid_missing_newline),
    CASE(invalid_newline_outside_length), CASE(invalid_trailing_space),
    CASE(unmounted), CASE(closed), CASE(isr), CASE(pending_boundary_preflight),
    CASE(queue_zero), CASE(short_write_repair), CASE(short_write_one),
    CASE(short_write_last_byte), CASE(boundary_no_capacity_retry),
    CASE(boundary_write_failure_retry), CASE(boundary_only_capacity),
    CASE(boundary_record_one_short), CASE(boundary_then_zero_write),
    CASE(boundary_then_short_write), CASE(flush_not_finished),
    CASE(flush_failure), CASE(flush_no_capacity), CASE(flush_zero_write),
    CASE(reentrant_capacity), CASE(reentrant_write), CASE(reentrant_flush),
    CASE(reentrant_zero_write), CASE(reentrant_no_capacity),
    CASE(reentrant_boundary_write), CASE(reentrant_boundary_record),
    CASE(reentrant_boundary_failure), CASE(reentrant_keeps_owner_lock)
};

#endif

int main(int argc, char **argv)
{
    s_case = argc == 2 ? argv[1] : "<missing case>";
    if (argc != 2) {
        fprintf(stderr, "usage: %s CASE (one case per process)\n", argv[0]);
        return EXIT_FAILURE;
    }
    reset_mock();
#if CONFIG_APP_USB_HID_ENABLE
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(s_case, cases[i].name) != 0) continue;
        cases[i].run();
        /* Every scenario ends with a fresh successful call. This observes
         * lock release without inspecting/resetting production static state.
         * It also detects an unwanted leftover boundary (extra queue call). */
        reset_mock();
        success_byte_count();
        printf("PASS %s (including next-call lock/boundary check)\n", s_case);
        return EXIT_SUCCESS;
    }
#else
    if (strcmp(s_case, "disabled") == 0) {
        CHECK(bsp_usb_tx_record("ok\n", 3, true) == ESP_ERR_NOT_SUPPORTED);
        CHECK(bsp_usb_tx_record(NULL, 0, false) == ESP_ERR_NOT_SUPPORTED);
        CHECK(bsp_usb_tx_record("\n", SIZE_MAX, true) == ESP_ERR_NOT_SUPPORTED);
        CHECK(m.isr_calls == 0 && m.mounted_calls == 0);
        CHECK(m.available_calls == 0 && m.write_calls == 0 && m.flush_calls == 0);
        puts("PASS disabled (no dependency calls)");
        return EXIT_SUCCESS;
    }
#endif
    fprintf(stderr, "unknown case: %s\n", s_case);
    return EXIT_FAILURE;
}
