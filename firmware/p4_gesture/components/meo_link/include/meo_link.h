#pragma once

/* meo_link —— 单根 USB 线上的 HID 双向链路
 *
 * 目标：让日志、命令、事件共用一条 HID 通道，并且不依赖 TinyUSB 或任何
 * 具体 MCU。宿主只需提供两个函数指针（ready / send），库负责分片、重组、
 * 队列、限流与统计。这样同一份实现可以搬到别的板子上，也可以用单元测试
 * 直接驱动。
 *
 * 报告格式（HID Report ID 3，共 64 字节）
 *
 *   byte 0        类型      'R' 命令回复 / 'L' 日志 / 'E' 事件 / 'C' 命令
 *   byte 1        msg_id    每条消息自增，主机据此发现丢消息
 *   byte 2        frag_idx  从 0 开始
 *   byte 3        frag_total
 *   byte 4..63    载荷      最多 60 字节，不要求 NUL 结尾
 *
 * 为什么带 msg_id 而不是当成纯字节流：HID 中断端点只有很浅的缓冲，
 * 主机来不及取时 tud_hid_report() 会失败，消息只能丢弃。有了 msg_id 和
 * frag_idx，主机能立刻知道"这条不完整，别拼"，而不是把两条消息的碎片
 * 粘成一行乱码。
 *
 * 上下行是两条独立通道，互不背压：CDC 写满不应阻止 HID 发送，反之亦然。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEO_LINK_REPORT_ID    3
#define MEO_LINK_REPORT_SIZE  64
#define MEO_LINK_HEADER_SIZE  4
#define MEO_LINK_PAYLOAD_SIZE (MEO_LINK_REPORT_SIZE - MEO_LINK_HEADER_SIZE)  /* 60 */

/* 单条消息上限。WIFI_SCAN 的整行结果在十几个 AP 时会超过 400 字节，
 * 512 留出的余量刚好覆盖它而不会让发送队列占太多内部 RAM。 */
#define MEO_LINK_MESSAGE_MAX  512

typedef enum {
    MEO_LINK_KIND_REPLY = 'R',   /* 命令回复，对应 CDC 上同一行文本 */
    MEO_LINK_KIND_LOG   = 'L',   /* ESP_LOG 镜像，受令牌桶限流 */
    MEO_LINK_KIND_EVENT = 'E',   /* 主动事件（状态变化、按键） */
} meo_link_kind_t;

/* 宿主提供的传输层。两个函数都必须是非阻塞的，且可以在任务上下文调用。 */
typedef struct {
    /* 设备已配置且端点可写。返回 false 时库会短暂等待而不是丢消息。 */
    bool (*ready)(void);
    /* 发送一个完整的 64 字节报告（不含 Report ID，由 USB 栈补）。 */
    bool (*send)(const uint8_t *report, size_t len);
} meo_link_transport_t;

typedef void (*meo_link_command_cb_t)(const char *line, void *ctx);

typedef struct {
    uint32_t tx_messages;    /* 成功发完的整条消息 */
    uint32_t tx_fragments;   /* 实际写入端点的分片数 */
    uint32_t tx_dropped;     /* 端点长期不可写，整条放弃 */
    uint32_t tx_retry;       /* 端点忙导致的单次重试 */
    uint32_t rx_messages;    /* 完整重组并派发的消息 */
    uint32_t rx_fragments;
    uint32_t rx_dropped;     /* 序号断裂、超长或类型非法 */
    uint32_t log_dropped;    /* 限流丢弃的日志行 */
    uint32_t log_enabled;    /* 1 = 正在镜像日志 */
} meo_link_stats_t;

/* 初始化并启动收发任务。transport 指向的结构必须保持有效（通常是静态的）。 */
esp_err_t meo_link_init(const meo_link_transport_t *transport);

/* 主机是否在线（转发 transport->ready）。 */
bool meo_link_ready(void);

/* 命令回调与 CDC 共用同一套语义：一行一条，不带换行符。 */
void meo_link_set_command_cb(meo_link_command_cb_t cb, void *ctx);

/* 主动发一条消息。队满返回 ESP_ERR_NO_MEM，调用方可以据此降级。 */
esp_err_t meo_link_send(meo_link_kind_t kind, const char *text);

/* 日志镜像；超过令牌桶速率时静默丢弃并累加 log_dropped。 */
esp_err_t meo_link_log(const char *text);

void meo_link_set_log_enabled(bool on);
bool meo_link_log_enabled(void);

/* 复位前调用。置位后所有发送立即返回失败，发送任务不再触碰 USB 端点。
 * 不删除任务：删除需要与可能正在进行的 tud_hid_report() 同步，而复位路径
 * 上多等一毫秒都可能让 PWR_HOLD 掉电。 */
void meo_link_quiesce(void);

/* 由 USB HID OUT 回调调用。只做字节重组，不执行业务逻辑，可安全地在
 * TinyUSB 回调里调用。 */
void meo_link_on_report(const uint8_t *data, size_t len);

/* LINK STATUS / LINK LOG / LINK PING 由链路自己处理，避免每个业务模块都
 * 重复实现。返回 true 表示这行已被消费。暴露出来是为了让宿主的命令分发
 * 能在最前面短路，而不必绕一圈。 */
bool meo_link_handle_builtin(const char *line);

void meo_link_get_stats(meo_link_stats_t *out);

#ifdef __cplusplus
}
#endif
