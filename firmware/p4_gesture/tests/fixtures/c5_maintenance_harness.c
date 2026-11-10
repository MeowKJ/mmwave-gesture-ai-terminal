/* Native shims; Python inserts the actual C5 maintenance decision functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "PROTOCOL_HEADER"
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_SUPPORTED };
enum { GPIO_PULLUP_ONLY=1, GPIO_PULLDOWN_ONLY=2, GPIO_MODE_INPUT_OUTPUT=3 };
enum { ESP_OTA_IMG_PENDING_VERIFY=1, ESP_OTA_IMG_VALID=2 };
#define C5_IO2_SHARED_GPIO 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static uint32_t boot_id=123, pin_mode;
static int64_t lease_deadline, clock_us=3000000;
static unsigned health_reads;
static uint32_t last_health_seq;
static bool last_health_valid;
static int state=ESP_OTA_IMG_PENDING_VERIFY, releases, pulls, drives, confirms, restarts;
static bool release_fail, pull_fail, drive_fail;
typedef struct { uint8_t app_elf_sha256[32]; } esp_app_desc_t;
static esp_app_desc_t descriptor;
static const esp_app_desc_t *esp_app_get_description(void) { return &descriptor; }
static int esp_ota_mark_app_valid_cancel_rollback(void) { confirms++; state=2; return ESP_OK; }
static int image_state(void) { return state; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void esp_restart(void) { restarts++; }
static int c5_io2_guard_apply(void) { releases++; return release_fail ? ESP_ERR_INVALID_STATE : ESP_OK; }
static int gpio_set_pull_mode(int pin, int mode) {
    assert(pin==2 && (mode==1 || mode==2)); pulls++;
    return pull_fail ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static int gpio_set_level(int pin, int level) { assert(pin==2 && (level==0 || level==1)); return ESP_OK; }
static int gpio_set_direction(int pin, int mode) {
    assert(pin==2 && mode==3); drives++; return drive_fail ? ESP_ERR_INVALID_STATE : ESP_OK;
}
/* ACTUAL_PIN_APPLY */
/* ACTUAL_DECISIONS */
static meo_c5_request_t request(unsigned op) {
    meo_c5_request_t r={.op=op,.boot_id=123,.seq=1}; return r;
}
static void three_health_reads(void) {
    meo_c5_request_t r=request(MEO_C5_INFO);
    for (r.seq=1;r.seq<=3;r.seq++) assert(handle_request(&r,clock_us)==0);
}
int main(int argc, char **argv) {
    assert(argc==2);
    const char *s=argv[1]; meo_c5_request_t r=request(MEO_C5_CONFIRM);
    if (!strcmp(s,"no_auto_confirm")) {
        three_health_reads(); assert(confirms==0 && state==1);
    } else if (!strcmp(s,"confirm")) {
        three_health_reads(); assert(handle_request(&r,clock_us)==0 && confirms==1 && state==2);
    } else if (!strcmp(s,"insufficient_health")) {
        assert(handle_request(&r,clock_us)==ESP_ERR_INVALID_STATE && confirms==0);
    } else if (!strcmp(s,"stale_nonce")) {
        three_health_reads(); r.boot_id=124;
        assert(handle_request(&r,clock_us)==ESP_ERR_INVALID_STATE && confirms==0);
    } else if (!strcmp(s,"wrong_hash")) {
        three_health_reads(); r.elf_sha[0]=1;
        assert(handle_request(&r,clock_us)==ESP_ERR_INVALID_STATE && confirms==0);
    } else if (!strcmp(s,"too_early")) {
        three_health_reads(); assert(handle_request(&r,1000000)==ESP_ERR_INVALID_STATE && confirms==0);
    } else if (!strcmp(s,"pin_prevents_confirm")) {
        three_health_reads(); assert(pin_apply(MEO_C5_PULLUP,500)==0);
        assert(handle_request(&r,clock_us)==ESP_ERR_INVALID_STATE && confirms==0);
    } else if (!strcmp(s,"duplicate_health")) {
        r=request(MEO_C5_INFO);
        for (int i=0;i<3;i++) assert(handle_request(&r,clock_us)==0);
        assert(health_reads==1);
    } else if (!strcmp(s,"lease_expires")) {
        assert(pin_apply(MEO_C5_PULLUP,500)==0 && pulls==1 && pin_mode==1);
        advance_timers(clock_us+499000); assert(pin_mode==1);
        advance_timers(clock_us+500000); assert(pin_mode==0 && lease_deadline==0 && releases==2);
    } else if (!strcmp(s,"lease_bounds")) {
        assert(pin_apply(1,99)==ESP_ERR_INVALID_ARG && pin_apply(1,5001)==ESP_ERR_INVALID_ARG);
        assert(pin_apply(5,500)==ESP_ERR_INVALID_ARG && releases==0 && pulls==0 && drives==0);
    } else if (!strcmp(s,"break_before_make")) {
        assert(pin_apply(3,500)==0 && releases==1 && drives==1);
        assert(pin_apply(4,500)==0 && releases==2 && drives==2);
        assert(pin_apply(0,0)==0 && releases==3 && pin_mode==0);
    } else if (!strcmp(s,"pull_failure")) {
        pull_fail=true; assert(pin_apply(1,500)==ESP_ERR_INVALID_STATE);
        assert(pin_mode==0 && lease_deadline==0 && releases==2);
    } else if (!strcmp(s,"release_failure")) {
        assert(pin_apply(1,500)==0); release_fail=true;
        advance_timers(clock_us+500000); assert(restarts==1 && drives==0);
    } else if (!strcmp(s,"unconfirmed_timeout")) {
        advance_timers(91000000); assert(restarts==1 && confirms==0 && pin_mode==0);
    } else if (!strcmp(s,"valid_no_timeout")) {
        state=2; advance_timers(91000000); assert(restarts==0);
    } else if (!strcmp(s,"stale_pin_request")) {
        r=request(MEO_C5_PIN); r.boot_id=124; r.arg0=4; r.arg1=500;
        assert(handle_request(&r,clock_us)==ESP_ERR_INVALID_STATE && drives==0 && releases==0);
    } else { return 2; }
    return 0;
}
