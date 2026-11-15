#include "meoave_voice_link.h"

#include "bsp_usb.h"
#include "meoave_sentence.h"
#include "meoave_voice.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

/* CDC is a bulk stream, not a message transport.  Every frame therefore has
 * an explicit little-endian header and CRC.  The payload is intentionally
 * below the 64-byte read size used by the USB callback's first iteration; a
 * frame may still be split across any number of USB packets. */
#define VOICE_MAGIC0 'M'
#define VOICE_MAGIC1 'V'
#define VOICE_MAGIC2 'O'
#define VOICE_MAGIC3 '1'
#define VOICE_VERSION 1u
#define VOICE_HEADER_BYTES 24u
#define VOICE_MAX_PAYLOAD MEO_VOICE_STREAM_BLOCK_BYTES
#define VOICE_META_BYTES MEO_SENTENCE_JSON_BYTES

enum {
    VOICE_FRAME_META = 1,
    VOICE_FRAME_PCM = 2,
    VOICE_FRAME_END = 3,
    VOICE_FRAME_CANCEL = 4,
};

enum {
    VOICE_FLAG_FIRST = 1u << 0,
    VOICE_FLAG_LAST = 1u << 1,
};

static uint8_t s_header[VOICE_HEADER_BYTES];
static uint8_t s_payload[VOICE_MAX_PAYLOAD];
static size_t s_header_used;
static size_t s_payload_used;
static uint16_t s_payload_len;
static bool s_open;
static bool s_sentence_ready;
static uint32_t s_generation;
static uint32_t s_sentence_id;
static uint32_t s_next_seq;
static char s_token[17];
static char s_meta[VOICE_META_BYTES + 1];
static size_t s_meta_len;
static StreamBufferHandle_t s_rx_stream;
static TaskHandle_t s_rx_task;
static volatile bool s_parser_overflow;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t crc16_update(uint16_t crc, uint8_t value)
{
    crc ^= (uint16_t)value << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    return crc;
}

static uint16_t frame_crc(void)
{
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < VOICE_HEADER_BYTES; ++i) {
        uint8_t value = (i == 22 || i == 23) ? 0 : s_header[i];
        crc = crc16_update(crc, value);
    }
    for (size_t i = 0; i < s_payload_len; ++i) crc = crc16_update(crc, s_payload[i]);
    return crc;
}

static void reset_parser(void)
{
    s_header_used = 0;
    s_payload_used = 0;
    s_payload_len = 0;
}

static void close_session(void)
{
    s_open = false;
    s_sentence_ready = false;
    s_meta_len = 0;
    reset_parser();
    bsp_usb_set_binary_rx(false);
}

static void reply(const char *status, const char *reason)
{
    char line[192];
    if (reason && reason[0]) {
        snprintf(line, sizeof(line), "MEO:VOICE_%s token=%s reason=%s\n",
                 status, s_token, reason);
    } else {
        snprintf(line, sizeof(line), "MEO:VOICE_%s token=%s\n", status, s_token);
    }
    (void)bsp_usb_cdc_write(line);
}

/* A native USB CDC write may complete into the Windows driver before the
 * bytes have actually reached the device.  The host therefore uses an
 * explicit per-frame acceptance as flow control instead of relying on sleep
 * based pacing.  Keep this acknowledgement short and machine-readable so it
 * can share the CDC log stream without becoming a second protocol. */
static void reply_pcm_accepted(uint32_t sequence)
{
    char line[128];
    snprintf(line, sizeof(line),
             "MEO:VOICE_PCM_ACCEPTED token=%s seq=%lu\n",
             s_token, (unsigned long)sequence);
    (void)bsp_usb_cdc_write(line);
}

static bool frame_identity_ok(void)
{
    return s_open && get_u32(s_header + 8) == s_generation &&
           get_u32(s_header + 12) == s_sentence_id &&
           get_u32(s_header + 16) == s_next_seq;
}

static void reject(const char *reason)
{
    reply("NACK", reason);
    (void)meo_voice_cancel();
    close_session();
}

static void handle_frame(void)
{
    /* The ASCII OPEN command deliberately carries no sentence metadata.  The
       first META frame establishes the generation/sentence identity; all
       following frames must match it exactly. */
    if (s_next_seq == 0 && s_header[5] == VOICE_FRAME_META &&
        (s_header[6] & VOICE_FLAG_FIRST)) {
        s_generation = get_u32(s_header + 8);
        s_sentence_id = get_u32(s_header + 12);
        if (!s_generation || !s_sentence_id) { reject("BAD_ID"); return; }
    }
    if (s_header[0] != VOICE_MAGIC0 || s_header[1] != VOICE_MAGIC1 ||
        s_header[2] != VOICE_MAGIC2 || s_header[3] != VOICE_MAGIC3 ||
        s_header[4] != VOICE_VERSION || s_payload_len > VOICE_MAX_PAYLOAD ||
        s_payload_used != s_payload_len || get_u16(s_header + 22) != frame_crc() ||
        !frame_identity_ok()) {
        reject("BAD_FRAME");
        return;
    }

    uint8_t kind = s_header[5];
    uint8_t flags = s_header[6];
    bool ok = false;
    if (kind == VOICE_FRAME_META) {
        if (s_sentence_ready || (s_meta_len == 0 && !(flags & VOICE_FLAG_FIRST)) ||
            (s_meta_len && (flags & VOICE_FLAG_FIRST)) ||
            s_meta_len + s_payload_len > VOICE_META_BYTES) {
            reject("BAD_META");
            return;
        }
        memcpy(s_meta + s_meta_len, s_payload, s_payload_len);
        s_meta_len += s_payload_len;
        if (flags & VOICE_FLAG_LAST) {
            meo_sentence_t sentence;
            s_meta[s_meta_len] = '\0';
            bool parsed = meo_sentence_parse(s_meta, s_meta_len, &sentence);
            if (!parsed) { reject("META_PARSE"); return; }
            if (sentence.generation != s_generation || sentence.sentence_id != s_sentence_id) {
                reject("META_ID"); return;
            }
            ok = meo_voice_stream_begin_sentence(&sentence);
            if (!ok) {
                char reason[64];
                snprintf(reason, sizeof(reason), "META_%s", meo_voice_stream_start_error());
                reject(reason);
                return;
            }
            s_sentence_ready = true;
        } else {
            ok = true;
        }
    } else if (kind == VOICE_FRAME_PCM) {
        ok = s_sentence_ready && !(flags & (VOICE_FLAG_FIRST | VOICE_FLAG_LAST)) &&
             meo_voice_stream_push_pcm(s_payload, s_payload_len);
        if (!ok) { reject("PCM_BACKPRESSURE"); return; }
    } else if (kind == VOICE_FRAME_END) {
        ok = s_sentence_ready && s_payload_len == 0 && (flags & VOICE_FLAG_LAST) &&
             meo_voice_stream_end();
        if (!ok) { reject("BAD_END"); return; }
        reply("END_ACCEPTED", NULL);
        close_session();
    } else if (kind == VOICE_FRAME_CANCEL) {
        (void)meo_voice_cancel();
        reply("CANCELLED", NULL);
        close_session();
        return;
    } else {
        reject("UNKNOWN_FRAME");
        return;
    }
    if (ok) {
        uint32_t accepted_sequence = get_u32(s_header + 16);
        ++s_next_seq;
        if (kind == VOICE_FRAME_PCM) reply_pcm_accepted(accepted_sequence);
    }
}

static void parse_bytes(const uint8_t *data, size_t len)
{
    if (!data || !len || !s_open) return;
    while (len) {
        if (s_header_used < VOICE_HEADER_BYTES) {
            size_t take = VOICE_HEADER_BYTES - s_header_used;
            if (take > len) take = len;
            memcpy(s_header + s_header_used, data, take);
            s_header_used += take; data += take; len -= take;
            if (s_header_used < VOICE_HEADER_BYTES) continue;
            s_payload_len = get_u16(s_header + 20);
            if (s_payload_len > VOICE_MAX_PAYLOAD) { reject("PAYLOAD_TOO_LARGE"); return; }
        }
        if (s_payload_used < s_payload_len) {
            size_t take = s_payload_len - s_payload_used;
            if (take > len) take = len;
            memcpy(s_payload + s_payload_used, data, take);
            s_payload_used += take; data += take; len -= take;
        }
        if (s_payload_used == s_payload_len) {
            handle_frame();
            if (!s_open) return;
            reset_parser();
        }
    }
}

/* TinyUSB invokes the CDC callback from its small USB task.  JSON parsing,
 * cJSON allocation and starting the audio worker do not belong on that stack:
 * the old direct callback path could overflow it as soon as a native META
 * frame arrived, resetting P4 before the first PCM block.  Copy the byte
 * stream into a bounded buffer and parse it from a dedicated worker instead.
 */
static void on_bytes(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!data || !len || !s_rx_stream) return;
    if (xStreamBufferSend(s_rx_stream, data, len, 0) != len) {
        /* The parser task will see the session still open and reject the
         * next complete frame; never block the USB callback on backpressure. */
        s_parser_overflow = true;
    }
}

static void voice_link_task(void *arg)
{
    (void)arg;
    uint8_t buffer[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_rx_stream, buffer, sizeof(buffer),
                                        portMAX_DELAY);
        if (n) {
            if (s_parser_overflow) {
                s_parser_overflow = false;
                reject("RX_OVERFLOW");
            } else {
                parse_bytes(buffer, n);
            }
        }
    }
}

void meo_voice_link_init(void)
{
    meo_voice_stream_init();
    if (!s_rx_stream) s_rx_stream = xStreamBufferCreate(4096, 1);
    if (s_rx_stream && !s_rx_task)
        (void)xTaskCreate(voice_link_task, "voice_link", 4096, NULL, 5, &s_rx_task);
    bsp_usb_set_binary_rx_callback(on_bytes, NULL);
}

static bool token_ok(const char *token)
{
    if (!token || !token[0]) return false;
    for (const char *p = token; *p; ++p)
        if (!(('0' <= *p && *p <= '9') || ('a' <= *p && *p <= 'z') ||
              ('A' <= *p && *p <= 'Z') || *p == '_' || *p == '-')) return false;
    return true;
}

bool meo_voice_link_command(const char *line)
{
    if (!line || strncmp(line, "VOICE ", 6) != 0) return false;
    char verb[16] = {0}, token[17] = {0};
    if (sscanf(line, "VOICE %15s %16s", verb, token) < 1) return true;
    if (strcmp(verb, "OPEN") == 0) {
        if (!token_ok(token) || s_open || meo_voice_stream_active()) {
            snprintf(s_token, sizeof(s_token), "%s", token[0] ? token : "none");
            char line_out[192];
            snprintf(line_out, sizeof(line_out), "MEO:VOICE_OPEN token=%s accepted=0 reason=BUSY_OR_BAD_TOKEN\n", s_token);
            (void)bsp_usb_cdc_write(line_out);
            return true;
        }
        memset(s_token, 0, sizeof(s_token));
        snprintf(s_token, sizeof(s_token), "%s", token);
        s_generation = 0; s_sentence_id = 0; s_next_seq = 0;
        s_meta_len = 0; s_sentence_ready = false; reset_parser(); s_open = true;
        bsp_usb_set_binary_rx(true);
        char line_out[128];
        snprintf(line_out, sizeof(line_out), "MEO:VOICE_OPEN token=%s accepted=1\n", s_token);
        (void)bsp_usb_cdc_write(line_out);
    } else if (strcmp(verb, "CANCEL") == 0) {
        (void)meo_voice_cancel();
        close_session();
        reply("CANCELLED", NULL);
    } else if (strcmp(verb, "STATUS") == 0) {
        reply(meo_voice_stream_active() ? "BUSY" : "IDLE", NULL);
    } else {
        reply("NACK", "UNKNOWN_COMMAND");
    }
    return true;
}
