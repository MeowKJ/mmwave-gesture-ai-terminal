#include "meo_link.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* 队列深度。TX 存"消息池下标"，RX 存原始报告帧。
 * 8 条消息足够覆盖"扫到十几个 AP 后连发几条回复"的突发，而不会让内部
 * RAM 为一条很少走的路径付出太多。 */
#define TX_QUEUE_DEPTH   8
#define RX_QUEUE_DEPTH   16

#define TX_TASK_STACK    3584
#define TX_TASK_PRIO     4
/* 命令回调运行在这个任务里，深度与 CDC 侧一致。曾经用 4096，但命令回复
 * 路径会一路调用到各 BSP，实测不足以覆盖最深的 AUDIO/WIFI 分支。 */
#define RX_TASK_STACK    6144
#define RX_TASK_PRIO     5

/* 端点持续不可写时最多等待多久。超过就放弃整条消息，避免 TX 任务被
 * 一个拔掉的线缆永久占住。 */
#define SEND_RETRY_MAX   20
#define SEND_RETRY_MS    2

/* 日志限流：40 行/秒、突发 8 行。ESP_LOG 在 Wi-Fi 扫描时很吵，不限制会
 * 把命令回复挤到几秒之后。 */
#define LOG_BURST        8
#define LOG_REFILL_PER_S 40

typedef struct {
    uint8_t kind;
    uint16_t len;
    char text[MEO_LINK_MESSAGE_MAX];
} tx_msg_t;

static const meo_link_transport_t *s_transport;

/* 消息池而不是值队列。
 *
 * 值队列要求每个生产者在自己栈上先构造一个 516 字节的结构体再入队。
 * meo_link_send() 会被"命令回复"路径调用，而那条路径的栈本来就被各 BSP
 * 的 reply 缓冲吃掉大半；再叠 516 字节就会在 4 KB 任务栈上溢出（实测表现
 * 是 RISC-V store access fault + ESP_RST_PANIC）。
 *
 * 改成：生产者只从空闲队列领一个下标，往静态池里写，再把下标交给发送
 * 任务。调用方的栈开销降到几个字节。 */
static tx_msg_t s_pool[TX_QUEUE_DEPTH];
static QueueHandle_t s_free_idx;
static QueueHandle_t s_ready_idx;
static QueueHandle_t s_rxq;
/* 复位过程中必须停止碰 USB 端点：那时栈正在被拆掉。 */
static volatile bool s_quiesced;

static meo_link_command_cb_t s_cmd_cb;
static void *s_cmd_ctx;

static meo_link_stats_t s_stats;
static volatile bool s_log_on = true;
static volatile bool s_started;

/* TX 分片序号。只有 TX 任务读写，RX 侧独立编号。 */
static uint8_t s_tx_id;

/* RX 重组状态，只在 RX 任务里访问。 */
static uint8_t s_rx_id;
static uint8_t s_rx_total;
static uint8_t s_rx_next;
static size_t  s_rx_len;
static bool    s_rx_active;
static char    s_rx_buf[MEO_LINK_MESSAGE_MAX + 1];

/* 日志令牌桶。 */
static int32_t s_tokens = LOG_BURST;
static int64_t s_token_stamp;

/* ---------------------------------------------------------------- 下行 */

/* 把重组好的一整块文本按行派发。主机可以一次发多行，也可以把一行拆成
 * 多个分片——两种情况下这里看到的都是拼好的字节。 */
static void dispatch_text(char *text, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') --end;
            /* 空行没有意义，跳过但不报错：主机发分片时常在尾部补 \n。 */
            if (end > start) {
                char saved = text[end];
                text[end] = '\0';
                /* 内建命令先处理，避免业务回调需要知道链路细节。 */
                if (!meo_link_handle_builtin(text + start) && s_cmd_cb) {
                    s_cmd_cb(text + start, s_cmd_ctx);
                }
                text[end] = saved;
            }
            start = i + 1;
        }
    }
}

static void rx_reassemble(const uint8_t *frame)
{
    uint8_t kind  = frame[0];
    uint8_t id    = frame[1];
    uint8_t index = frame[2];
    uint8_t total = frame[3];

    if (kind != (uint8_t)MEO_LINK_KIND_REPLY && kind != (uint8_t)'C') {
        ++s_stats.rx_dropped;
        return;
    }
    if (total == 0 || total > MEO_LINK_MAX_FRAGMENTS || index >= total) {
        ++s_stats.rx_dropped;
        return;
    }
    ++s_stats.rx_fragments;

    if (index == 0) {
        s_rx_active = true;
        s_rx_id = id;
        s_rx_total = total;
        s_rx_next = 0;
        s_rx_len = 0;
    } else if (!s_rx_active || id != s_rx_id || index != s_rx_next || total != s_rx_total) {
        /* 中间分片丢失，或上一条消息没发完就开始了新的一条。整条丢弃：
         * 拼出来的半行命令比没有命令更危险。 */
        s_rx_active = false;
        ++s_stats.rx_dropped;
        return;
    }

    size_t room = sizeof(s_rx_buf) - 1 - s_rx_len;
    size_t take = MEO_LINK_PAYLOAD_SIZE;
    if (take > room) take = room;
    if (take) memcpy(s_rx_buf + s_rx_len, frame + MEO_LINK_HEADER_SIZE, take);
    s_rx_len += take;
    s_rx_next = (uint8_t)(index + 1);

    if (s_rx_next == s_rx_total) {
        /* Reports are fixed-size. The last fragment is zero padded by the
         * desktop HID client; NUL is not valid in a command line, so remove
         * only the padding at the end before dispatching. */
        while (s_rx_len && s_rx_buf[s_rx_len - 1] == '\0') --s_rx_len;
        s_rx_buf[s_rx_len] = '\0';
        s_rx_active = false;
        ++s_stats.rx_messages;
        dispatch_text(s_rx_buf, s_rx_len);
    }
}

void meo_link_on_report(const uint8_t *data, size_t len)
{
    if (!data || len < MEO_LINK_HEADER_SIZE || !s_rxq) return;
    uint8_t frame[MEO_LINK_REPORT_SIZE] = {0};
    size_t n = len < MEO_LINK_REPORT_SIZE ? len : MEO_LINK_REPORT_SIZE;
    memcpy(frame, data, n);
    /* 不等待：TinyUSB 回调里阻塞会拖垮整个 USB 栈。丢掉一帧，主机的
     * msg_id 检查会发现。 */
    if (xQueueSend(s_rxq, frame, 0) != pdTRUE) ++s_stats.rx_dropped;
}

/* ---------------------------------------------------------------- 上行 */

static bool emit_report(const uint8_t *report)
{
    if (s_quiesced) return false;
    if (!s_transport || !s_transport->ready || !s_transport->send) return false;
    for (int attempt = 0; attempt < SEND_RETRY_MAX; ++attempt) {
        if (s_quiesced) return false;
        if (!s_transport->ready()) {
            vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_MS));
            continue;
        }
        if (s_transport->send(report, MEO_LINK_REPORT_SIZE)) return true;
        ++s_stats.tx_retry;
        vTaskDelay(pdMS_TO_TICKS(SEND_RETRY_MS));
    }
    return false;
}

static void send_message(const tx_msg_t *msg)
{
    size_t total = (msg->len + MEO_LINK_PAYLOAD_SIZE - 1) / MEO_LINK_PAYLOAD_SIZE;
    if (total == 0) total = 1;                 /* 空消息也要占一帧，用于探测 */
    if (total > MEO_LINK_MAX_FRAGMENTS) {
        ++s_stats.tx_dropped;
        return;
    }

    uint8_t id = s_tx_id++;
    for (size_t i = 0; i < total; ++i) {
        uint8_t report[MEO_LINK_REPORT_SIZE] = {0};
        report[0] = msg->kind;
        report[1] = id;
        report[2] = (uint8_t)i;
        report[3] = (uint8_t)total;

        size_t offset = i * MEO_LINK_PAYLOAD_SIZE;
        if (offset < msg->len) {
            size_t chunk = msg->len - offset;
            if (chunk > MEO_LINK_PAYLOAD_SIZE) chunk = MEO_LINK_PAYLOAD_SIZE;
            memcpy(report + MEO_LINK_HEADER_SIZE, msg->text + offset, chunk);
        }
        if (!emit_report(report)) {
            ++s_stats.tx_dropped;
            return;
        }
        ++s_stats.tx_fragments;
    }
    ++s_stats.tx_messages;
}

/* ---------------------------------------------------------------- 任务 */

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t index;
    for (;;) {
        if (xQueueReceive(s_ready_idx, &index, portMAX_DELAY) != pdTRUE) continue;
        send_message(&s_pool[index]);
        /* 无论发送成功与否都要归还，否则连续失败会把池耗尽。 */
        (void)xQueueSend(s_free_idx, &index, 0);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t frame[MEO_LINK_REPORT_SIZE];
    for (;;) {
        if (xQueueReceive(s_rxq, frame, portMAX_DELAY) == pdTRUE) rx_reassemble(frame);
    }
}

/* ---------------------------------------------------------------- 内建命令 */

bool meo_link_handle_builtin(const char *line);

bool meo_link_handle_builtin(const char *line)
{
    /* LINK 前缀保留给链路自身，业务侧不应再处理。 */
    if (strncmp(line, "LINK ", 5) != 0) return false;

    char verb[12] = {0};
    char token[17] = {0};
    if (sscanf(line, "LINK %11s %16s", verb, token) != 2) return true;

    char reply[192];
    if (strcmp(verb, "STATUS") == 0) {
        meo_link_stats_t s;
        meo_link_get_stats(&s);
        snprintf(reply, sizeof(reply),
                 "MEO:LINK token=%s tx=%u frag=%u drop=%u retry=%u rx=%u rxd=%u log=%u logdrop=%u\n",
                 token, (unsigned)s.tx_messages, (unsigned)s.tx_fragments,
                 (unsigned)s.tx_dropped, (unsigned)s.tx_retry,
                 (unsigned)s.rx_messages, (unsigned)s.rx_dropped,
                 (unsigned)s.log_enabled, (unsigned)s.log_dropped);
        (void)meo_link_send(MEO_LINK_KIND_REPLY, reply);
    } else if (strcmp(verb, "LOG") == 0) {
        /* LINK LOG ON|OFF <token> */
        char state[8] = {0};
        if (sscanf(line, "LINK LOG %7s %16s", state, token) == 2) {
            bool on = strcmp(state, "ON") == 0;
            meo_link_set_log_enabled(on);
            snprintf(reply, sizeof(reply), "MEO:LINK token=%s log=%u\n",
                     token, on ? 1u : 0u);
        } else {
            snprintf(reply, sizeof(reply), "MEO:LINK token=%s reason=bad_args\n", token);
        }
        (void)meo_link_send(MEO_LINK_KIND_REPLY, reply);
    } else if (strcmp(verb, "PING") == 0) {
        snprintf(reply, sizeof(reply), "MEO:PONG token=%s via=hid\n", token);
        (void)meo_link_send(MEO_LINK_KIND_REPLY, reply);
    } else if (strcmp(verb, "CAPS") == 0) {
        snprintf(reply, sizeof(reply),
                 "MEO:LINK token=%s caps=1 version=1 report_id=%u report_size=%u "
                 "payload=%u max_message=%u max_fragments=%u kinds=RLE transport=hid\n",
                 token, (unsigned)MEO_LINK_REPORT_ID,
                 (unsigned)MEO_LINK_REPORT_SIZE, (unsigned)MEO_LINK_PAYLOAD_SIZE,
                 (unsigned)MEO_LINK_MESSAGE_MAX, (unsigned)MEO_LINK_MAX_FRAGMENTS);
        (void)meo_link_send(MEO_LINK_KIND_REPLY, reply);
    } else {
        snprintf(reply, sizeof(reply), "MEO:LINK token=%s reason=unknown_op\n", token);
        (void)meo_link_send(MEO_LINK_KIND_REPLY, reply);
    }
    return true;
}

/* ---------------------------------------------------------------- 公共接口 */

esp_err_t meo_link_init(const meo_link_transport_t *transport)
{
    if (!transport || !transport->ready || !transport->send) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;

    s_transport = transport;
    s_free_idx = xQueueCreate(TX_QUEUE_DEPTH, sizeof(uint8_t));
    s_ready_idx = xQueueCreate(TX_QUEUE_DEPTH, sizeof(uint8_t));
    s_rxq = xQueueCreate(RX_QUEUE_DEPTH, MEO_LINK_REPORT_SIZE);
    if (!s_free_idx || !s_ready_idx || !s_rxq) return ESP_ERR_NO_MEM;

    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) (void)xQueueSend(s_free_idx, &i, 0);

    s_token_stamp = esp_timer_get_time();
    if (xTaskCreate(tx_task, "meo_link_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(rx_task, "meo_link_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    s_started = true;
    return ESP_OK;
}

bool meo_link_ready(void)
{
    return s_transport && s_transport->ready && s_transport->ready();
}

void meo_link_set_command_cb(meo_link_command_cb_t cb, void *ctx)
{
    s_cmd_cb = cb;
    s_cmd_ctx = ctx;
}

esp_err_t meo_link_send(meo_link_kind_t kind, const char *text)
{
    if (!s_started || !text || s_quiesced) return ESP_ERR_INVALID_STATE;

    size_t len = strlen(text);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) --len;
    if (len > MEO_LINK_MESSAGE_MAX) len = MEO_LINK_MESSAGE_MAX;

    /* 从池里领一块；领不到就说明积压了，直接放弃而不是阻塞调用方。 */
    uint8_t index;
    if (xQueueReceive(s_free_idx, &index, 0) != pdTRUE) return ESP_ERR_NO_MEM;

    tx_msg_t *msg = &s_pool[index];
    msg->kind = (uint8_t)kind;
    msg->len = (uint16_t)len;
    if (len) memcpy(msg->text, text, len);

    if (xQueueSend(s_ready_idx, &index, 0) != pdTRUE) {
        (void)xQueueSend(s_free_idx, &index, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void meo_link_quiesce(void)
{
    /* 复位前调用：让发送任务立刻停止触碰 USB 端点。这里只置标志，不删
     * 任务——删任务需要同步它当前可能正在进行的 tud_hid_report()。 */
    s_quiesced = true;
}

esp_err_t meo_link_log(const char *text)
{
    if (!s_started || !s_log_on || !text) return ESP_ERR_INVALID_STATE;

    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_token_stamp;
    if (elapsed >= 25000) {                   /* 每 25 ms 补一个令牌 */
        int32_t refill = (int32_t)(elapsed / 25000);
        s_tokens += refill;
        if (s_tokens > LOG_BURST) s_tokens = LOG_BURST;
        s_token_stamp += (int64_t)refill * 25000;
    }
    if (s_tokens <= 0) {
        ++s_stats.log_dropped;
        return ESP_ERR_NO_MEM;
    }
    --s_tokens;
    return meo_link_send(MEO_LINK_KIND_LOG, text);
}

void meo_link_set_log_enabled(bool on)
{
    s_log_on = on;
    s_stats.log_enabled = on ? 1u : 0u;
}

bool meo_link_log_enabled(void)
{
    return s_log_on;
}

void meo_link_get_stats(meo_link_stats_t *out)
{
    if (!out) return;
    *out = s_stats;
    /* 这两个由调用方在别处更新，读出时补齐当前值。 */
    out->log_enabled = s_log_on ? 1u : 0u;
}
