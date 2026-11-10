/* Native test doubles only. The Python test inserts production handler source. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef uint32_t esp_ota_handle_t;
typedef void *TimerHandle_t;
typedef struct { uint32_t address, size; int type; const char *label; } esp_partition_t;
typedef struct { uint8_t data[24]; } esp_image_header_t;
typedef struct { uint8_t data[8]; } esp_image_segment_header_t;
typedef struct { uint8_t data[256]; } esp_app_desc_t;
typedef struct { int resp; } TestResp;
typedef TestResp RpcRespOTABegin;
typedef TestResp RpcRespOTAWrite;
typedef TestResp RpcRespOTAEnd;
typedef TestResp RpcRespOTAActivate;
typedef struct { struct { uint8_t *data; size_t len; } ota_data; } TestWrite;
typedef struct {
    int payload_case;
    TestWrite *req_ota_write;
    TestResp *resp_ota_begin, *resp_ota_write, *resp_ota_end, *resp_ota_activate;
} Rpc;

enum { ESP_OK=0, SUCCESS=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1,
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_NOT_FOUND,
    ESP_ERR_NO_MEM, ESP_ERR_OTA_PARTITION_CONFLICT, ESP_ERR_OTA_VALIDATE_FAILED };
enum { RPC__PAYLOAD_RESP_OTA_BEGIN=1, RPC__PAYLOAD_RESP_OTA_WRITE,
    RPC__PAYLOAD_RESP_OTA_END, RPC__PAYLOAD_RESP_OTA_ACTIVATE };
#define OTA_SIZE_UNKNOWN UINT32_MAX
#define RESTART_TIMEOUT 2000
#define pdFALSE 0
#define pdPASS 1
#define CONFIG_ESP_HOSTED_CP_WIFI 1
static const char *TAG = "test";
static void log_message(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(...) log_message(__VA_ARGS__)
#define ESP_LOGW(...) log_message(__VA_ARGS__)
#define ESP_LOGE(...) log_message(__VA_ARGS__)
static const char *esp_err_to_name(int err) { (void)err; return "injected"; }
#define rpc__resp__otabegin__init(p) ((void)(p))
#define rpc__resp__otawrite__init(p) ((void)(p))
#define rpc__resp__otaend__init(p) ((void)(p))
#define rpc__resp__otaactivate__init(p) ((void)(p))

static esp_partition_t running = {0x20000, 0x1e0000, 0, "ota_0"};
static esp_partition_t passive = {0x200000, 0x1e0000, 0, "ota_1"};
static const esp_partition_t *boot = &running;
static bool missing_next, missing_running, missing_boot, conflict, calloc_fail, timer_oom;
static int begin_err, write_err, validity_err, end_err, select_err, restore_err;
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static int confirm_err, confirms;
static int esp_ota_mark_app_valid_cancel_rollback(void) { confirms++; return confirm_err; }
#endif
static bool timer_fail;
static int begins, aborts, writes, ends, checks, selections, creates, starts, deletes, restarts;
static bool sdk_open;
static int timer_object;
static const esp_partition_t *esp_ota_get_next_update_partition(const void *p) {
    (void)p; return missing_next ? NULL : conflict ? &running : &passive;
}
static const esp_partition_t *esp_ota_get_running_partition(void) { return missing_running ? NULL : &running; }
static const esp_partition_t *esp_ota_get_boot_partition(void) { return missing_boot ? NULL : boot; }
static int esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) {
    assert(p == &passive && n == OTA_SIZE_UNKNOWN && !sdk_open);
    begins++; if (begin_err) return begin_err;
    *h = 42; sdk_open = true; return ESP_OK;
}
static int esp_ota_abort(esp_ota_handle_t h) {
    assert(h == 42 && sdk_open); sdk_open=false; aborts++; return ESP_OK;
}
static int esp_ota_write(esp_ota_handle_t h, const void *p, size_t n) {
    assert(h == 42 && sdk_open && p && n); writes++; return write_err;
}
static int esp_ota_end(esp_ota_handle_t h) {
    assert(h == 42 && sdk_open); sdk_open=false; ends++; return end_err;
}
#if H_OTA_CHECK_IMAGE_VALIDITY
static int esp_ota_check_image_validity(int type, const esp_image_header_t *h, const esp_app_desc_t *a) {
    (void)type; assert(h && a); checks++; return validity_err;
}
#endif
static int esp_ota_set_boot_partition(const esp_partition_t *p) {
    selections++; int err = p == &running ? restore_err : select_err;
    if (!err) boot=p;
    return err;
}
static TimerHandle_t xTimerCreate(const char *s, int period, int reload, int id, void (*cb)(TimerHandle_t)) {
    (void)s; (void)id; assert(period == 2000 && !reload && cb); creates++;
    return timer_oom ? NULL : &timer_object;
}
static int xTimerStart(TimerHandle_t t, int ticks) {
    (void)ticks; assert(t == &timer_object); starts++; return timer_fail ? 0 : pdPASS;
}
static int xTimerDelete(TimerHandle_t t, int ticks) {
    (void)ticks; assert(t == &timer_object); deletes++; return pdPASS;
}
static void esp_restart(void) { restarts++; }
typedef void (*shutdown_handler_t)(void);
static void esp_wifi_stop(void) {}
static void esp_unregister_shutdown_handler(shutdown_handler_t f) { assert(f == esp_wifi_stop); }
static void *test_calloc(size_t n, size_t size) { return calloc_fail ? NULL : calloc(n, size); }
#define calloc test_calloc

/* ACTUAL_OTA_CODE */

#undef calloc
static uint8_t packet[288];
static TestWrite payload = {{packet, sizeof(packet)}};
static Rpc request = {.req_ota_write=&payload};
typedef esp_err_t (*Handler)(Rpc *, Rpc *, void *);
static int invoke(Handler f) {
    Rpc response={0}; assert(f(&request, &response, NULL) == ESP_OK);
    TestResp *p = response.resp_ota_begin ? response.resp_ota_begin :
        response.resp_ota_write ? response.resp_ota_write :
        response.resp_ota_end ? response.resp_ota_end : response.resp_ota_activate;
    assert(p); int ret=p->resp; free(p); return ret;
}
static int begin(void) { return invoke(req_ota_begin_handler); }
static int write_chunk(void) { return invoke(req_ota_write_handler); }
static int end(void) { return invoke(req_ota_end_handler); }
static int activate(void) { return invoke(req_ota_activate_handler); }
static void complete(void) { assert(begin() == 0 && write_chunk() == 0 && end() == 0); }

int main(int argc, char **argv) {
    assert(argc == 2); const char *s=argv[1];
    if (!strcmp(s,"happy")) {
        complete(); assert(boot == &running && !sdk_open && checks == H_OTA_CHECK_IMAGE_VALIDITY);
        assert(activate() == 0 && boot == &passive && creates == 1 && starts == 1);
    } else if (!strcmp(s,"activate_idle")) { assert(activate() == ESP_ERR_INVALID_STATE && selections == 0);
    } else if (!strcmp(s,"write_idle")) { assert(write_chunk() == ESP_ERR_INVALID_STATE && writes == 0);
    } else if (!strcmp(s,"end_idle")) { assert(end() == ESP_ERR_INVALID_STATE && ends == 0);
    } else if (!strcmp(s,"begin_twice")) { assert(begin() == 0 && begin() == 0 && aborts == 1 && begins == 2);
    } else if (!strcmp(s,"begin_missing_slot")) { missing_next=true; assert(begin() == ESP_ERR_NOT_FOUND && begins == 0);
    } else if (!strcmp(s,"begin_sdk_failure")) { begin_err=91; assert(begin() == 91 && ota_status == OTA_FAILED && !ota_handle_open);
    } else if (!strcmp(s,"begin_running_slot")) { conflict=true; assert(begin() == ESP_ERR_OTA_PARTITION_CONFLICT && begins == 0);
    } else if (!strcmp(s,"begin_pending_boot")) { boot=&passive; assert(begin() == ESP_ERR_INVALID_STATE && begins == 0);
    } else if (!strcmp(s,"begin_missing_running")) { missing_running=true; assert(begin() == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(s,"begin_missing_boot")) { missing_boot=true; assert(begin() == ESP_ERR_INVALID_STATE);
    } else if (!strcmp(s,"first_short")) {
        assert(begin() == 0); payload.ota_data.len=1;
#if H_OTA_CHECK_IMAGE_VALIDITY
        assert(write_chunk() == ESP_ERR_INVALID_SIZE && writes == 0 && aborts == 1);
        assert(write_chunk() == ESP_ERR_INVALID_STATE && activate() == ESP_ERR_INVALID_STATE);
#else
        assert(write_chunk() == 0); /* Older IDF relies on end's image validation. */
#endif
    } else if (!strcmp(s,"first_invalid")) {
        assert(begin() == 0); validity_err=92;
#if H_OTA_CHECK_IMAGE_VALIDITY
        assert(write_chunk() == 92 && writes == 0 && aborts == 1);
        assert(write_chunk() == ESP_ERR_INVALID_STATE && activate() == ESP_ERR_INVALID_STATE);
#else
        assert(write_chunk() == 0);
#endif
    } else if (!strcmp(s,"write_failure")) { assert(begin() == 0); write_err=93;
        assert(write_chunk() == 93 && aborts == 1 && !ota_handle_open && ota_status == OTA_FAILED);
    } else if (!strcmp(s,"write_null_payload")) { assert(begin() == 0); request.req_ota_write=NULL;
        assert(write_chunk() == ESP_ERR_INVALID_ARG && aborts == 1);
    } else if (!strcmp(s,"write_null_data")) { assert(begin() == 0); payload.ota_data.data=NULL;
        assert(write_chunk() == ESP_ERR_INVALID_ARG && aborts == 1);
    } else if (!strcmp(s,"write_empty")) { assert(begin() == 0); payload.ota_data.len=0;
        assert(write_chunk() == ESP_ERR_INVALID_ARG && aborts == 1);
    } else if (!strcmp(s,"write_overflow")) { assert(begin() == 0); ota_bytes_written=passive.size-1;
        assert(write_chunk() == ESP_ERR_INVALID_SIZE && writes == 0 && aborts == 1);
    } else if (!strcmp(s,"end_empty")) { assert(begin() == 0);
        assert(end() == ESP_ERR_INVALID_SIZE && ends == 0 && aborts == 1);
    } else if (!strcmp(s,"end_invalid_image")) { assert(begin() == 0 && write_chunk() == 0); end_err=ESP_ERR_OTA_VALIDATE_FAILED;
        assert(end() == end_err && !ota_handle_open && aborts == 0);
        assert(activate() == ESP_ERR_INVALID_STATE && selections == 0);
    } else if (!strcmp(s,"repeat_end")) { complete(); assert(end() == 0 && ends == 1);
    } else if (!strcmp(s,"activate_in_progress")) { assert(begin() == 0);
        assert(activate() == ESP_ERR_INVALID_STATE && creates == 0);
    } else if (!strcmp(s,"activate_failed")) { assert(begin() == 0); write_err=1; (void)write_chunk();
        assert(activate() == ESP_ERR_INVALID_STATE && selections == 0);
    } else if (!strcmp(s,"timer_oom")) { complete(); timer_oom=true;
        assert(activate() == ESP_ERR_NO_MEM && selections == 0 && ota_status == OTA_COMPLETED);
    } else if (!strcmp(s,"boot_select_failure")) { complete(); select_err=94;
        assert(activate() == 94 && deletes == 1 && starts == 0 && ota_status == OTA_COMPLETED);
    } else if (!strcmp(s,"timer_failure")) { complete(); timer_fail=true;
        assert(activate() == ESP_FAIL && boot == &running && deletes == 1 && selections == 2);
        assert(ota_status == OTA_COMPLETED); timer_fail=false; assert(activate() == 0);
    } else if (!strcmp(s,"restore_failure")) { complete(); timer_fail=true; restore_err=95;
        assert(activate() == ESP_FAIL && ota_status == OTA_BOOT_SELECTED && boot == &passive);
        assert(activate() == ESP_ERR_INVALID_STATE && begin() == ESP_ERR_INVALID_STATE && begins == 1);
    } else if (!strcmp(s,"restore_confirm_failure")) {
        complete(); timer_fail=true;
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        confirm_err=96;
        assert(activate() == ESP_FAIL && confirms == 1 && ota_status == OTA_BOOT_SELECTED);
        assert(begin() == ESP_ERR_INVALID_STATE);
#else
        assert(activate() == ESP_FAIL && ota_status == OTA_COMPLETED);
#endif
    } else if (!strcmp(s,"repeat_activate")) { complete(); assert(activate() == 0 && activate() == 0 && creates == 1 && selections == 1);
    } else if (!strcmp(s,"begin_after_activate")) { complete(); assert(activate() == 0);
        assert(begin() == ESP_ERR_INVALID_STATE && begins == 1 && boot == &passive);
    } else if (!strcmp(s,"write_after_end")) { complete(); assert(write_chunk() == ESP_ERR_INVALID_STATE && writes == 1);
    } else if (!strcmp(s,"restart_callback")) { complete(); assert(activate() == 0);
        vTimerCallback(&timer_object); assert(restarts == 1 && deletes == 1);
    } else if (!strcmp(s,"null_request")) { Rpc r={0};
        Handler f[]={req_ota_begin_handler, req_ota_write_handler, req_ota_end_handler, req_ota_activate_handler};
        for (size_t i=0;i<sizeof(f)/sizeof(f[0]);i++) { assert(f[i](NULL,&r,NULL) == ESP_FAIL); assert(f[i](&request,NULL,NULL) == ESP_FAIL); }
    } else if (!strcmp(s,"response_oom")) { Rpc r={0}; calloc_fail=true;
        Handler f[]={req_ota_begin_handler, req_ota_write_handler, req_ota_end_handler, req_ota_activate_handler};
        for (size_t i=0;i<sizeof(f)/sizeof(f[0]);i++) assert(f[i](&request,&r,NULL) == ESP_ERR_NO_MEM);
        assert(begins == 0 && selections == 0);
    } else if (!strcmp(s,"new_begin_after_failure")) { assert(begin() == 0); write_err=93; assert(write_chunk() == 93);
        write_err=0; complete(); assert(ota_status == OTA_COMPLETED && begins == 2 && aborts == 1);
    } else if (!strcmp(s,"begin_after_completed")) { complete(); complete(); assert(begins == 2 && ends == 2 && boot == &running);
    } else { fprintf(stderr,"Unknown case: %s\n",s); return 2; }
    return 0;
}
