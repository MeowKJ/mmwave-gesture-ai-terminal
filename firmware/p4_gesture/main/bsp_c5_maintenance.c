/* Native USB -> single worker -> tested Hosted maintenance state machine.
 * Never blocks TinyUSB/LVGL, writes straps, starts radar/audio, or auto-resets C5.
 * ATTACH is reset-free; only an explicit CONNECT requests one shared EN pulse. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "bsp_c5_maintenance.h"
#include "bsp_usb.h"
#include "c5_host_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"

typedef struct { char id[17]; char command[1360]; } request_t;
static QueueHandle_t requests;
static bool transport_ok;
static volatile int busy;

static bool output(const char *line)
{
    if (!transport_ok) return false;
    int64_t end=esp_timer_get_time()+2000000;
    do {
        if (bsp_usb_cdc_write(line)==ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (esp_timer_get_time()<end);
    transport_ok=false;
    return false;
}

static void worker(void *arg)
{
    (void)arg;
    request_t request;
    bool init_attempted=false;
    esp_err_t init_error=ESP_ERR_INVALID_STATE;
    for (;;) {
        if (xQueueReceive(requests,&request,portMAX_DELAY)!=pdTRUE) continue;
        transport_ok=true;
        char line[160];
        snprintf(line,sizeof(line),"MEO:NATIVE_BEGIN id=%s\n",request.id);
        if (output(line)) {
            if (!init_attempted) {
                init_attempted=true;
                init_error=meo_c5_host_init();
            }
            if (init_error==ESP_OK) meo_c5_host_execute(request.command,output);
            else {
                snprintf(line,sizeof(line),"MEO:NATIVE_INIT error=0x%x\n",(unsigned)init_error);
                (void)output(line);
            }
            bool delivered=transport_ok;
            /* This END is only a transport boundary, never an OTA success. */
            transport_ok=true;
            snprintf(line,sizeof(line),"MEO:NATIVE_END id=%s tx_ok=%u init_error=0x%x\n",
                     request.id,delivered,(unsigned)init_error);
            (void)output(line);
        }
        mbedtls_platform_zeroize(&request,sizeof(request));
        __sync_lock_release(&busy);
    }
}

void bsp_c5_maintenance_init(void)
{
    requests=xQueueCreate(1,sizeof(request_t));
    if (requests && xTaskCreate(worker,"c5_native",12288,NULL,2,NULL)!=pdPASS) {
        vQueueDelete(requests);
        requests=NULL;
    }
}

bool bsp_c5_maintenance_command(const char *line)
{
    if (strncmp(line,"C5M ",4)) return false;
    request_t request={0};
    const char *id=line+4,*space=strchr(id,' ');
    if (!space || space==id || space-id>16 || !space[1] || strlen(space+1)>=sizeof(request.command)) {
        (void)bsp_usb_cdc_write("MEO:NATIVE_REJECT reason=syntax\n");
        return true;
    }
    for (const char *p=id;p<space;p++) if (!isalnum((unsigned char)*p)) return true;
    memcpy(request.id,id,(size_t)(space-id));
    strcpy(request.command,space+1);
    /* Do not wait here: this is TinyUSB's callback, not the app worker. */
    bool acquired=requests && __sync_lock_test_and_set(&busy,1)==0;
    if (!acquired || xQueueSend(requests,&request,0)!=pdTRUE) {
        if (acquired) __sync_lock_release(&busy);
        char reply[96];
        snprintf(reply,sizeof(reply),"MEO:NATIVE_REJECT id=%s reason=busy_or_unavailable\n",request.id);
        (void)bsp_usb_cdc_write(reply);
    }
    mbedtls_platform_zeroize(&request,sizeof(request));
    return true;
}
