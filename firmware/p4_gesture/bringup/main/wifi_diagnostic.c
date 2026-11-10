/* Real P4 TCP/IP -> SDIO -> C5 Wi-Fi diagnostics. No embedded credentials,
 * Flash config writes, captive-portal bypass, TLS verification bypass, or
 * automatic reconnect/reset loops. Product UI and GPIO6 are not touched. */
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "wifi_diagnostic.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "mbedtls/platform_util.h"

#if !CONFIG_MBEDTLS_HAVE_TIME_DATE
#error "Wi-Fi HTTPS acceptance requires real certificate validity-date checks"
#endif

#define NET_UP BIT0
#define NET_DOWN BIT1
static EventGroupHandle_t events;
static esp_netif_t *sta;
static bool setup_done, initialized, started;
static uint16_t disconnect_reason;

static esp_err_t stage(const char *name, esp_err_t error)
{
    printf("MEO:NET_STAGE step=%s error=0x%x\n",name,(unsigned)error);
    return error;
}

static void net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event=data;
        disconnect_reason=event->reason;
        xEventGroupClearBits(events,NET_UP);
        xEventGroupSetBits(events,NET_DOWN);
    } else if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event=data;
        if (event->esp_netif!=sta) return;
        xEventGroupClearBits(events,NET_DOWN);
        xEventGroupSetBits(events,NET_UP);
    }
}

static esp_err_t start_wifi(void)
{
    esp_err_t error=ESP_OK;
    if (!setup_done) {
        error=esp_netif_init();
        if (error!=ESP_OK) return error;
        events=xEventGroupCreate();
        if (!events) return ESP_ERR_NO_MEM;
        sta=esp_netif_create_default_wifi_sta();
        if (!sta) return ESP_ERR_NO_MEM;
        error=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,net_event,NULL);
        if (error==ESP_OK) error=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,net_event,NULL);
        if (error!=ESP_OK) return error;
        setup_done=true;
    }
    if (!initialized) {
        wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable=0;
        error=stage("init_ram",esp_wifi_init(&cfg));
        if (error!=ESP_OK) return error;
        initialized=true;
        error=stage("storage_ram",esp_wifi_set_storage(WIFI_STORAGE_RAM));
        if (error==ESP_OK) error=stage("mode_sta",esp_wifi_set_mode(WIFI_MODE_STA));
        if (error!=ESP_OK) return error;
    }
    if (!started) {
        error=stage("start",esp_wifi_start());
        if (error!=ESP_OK) return error;
        started=true;
#if CONFIG_SLAVE_SOC_WIFI_SUPPORT_5G
        /* The C5 API requires esp_wifi_start BEFORE changing band mode. */
        error=stage("band_auto",esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
#endif
        if (error==ESP_OK) error=stage("power_save_none",esp_wifi_set_ps(WIFI_PS_NONE));
    }
    return error;
}

static esp_err_t decode(const char *hex, uint8_t *out, size_t min, size_t max)
{
    if (!hex) return ESP_ERR_INVALID_ARG;
    size_t len=strlen(hex);
    if ((len&1) || len/2<min || len/2>max) return ESP_ERR_INVALID_ARG;
    for (size_t i=0;i<len/2;++i) {
        if (!isxdigit((unsigned char)hex[i*2]) || !isxdigit((unsigned char)hex[i*2+1])) return ESP_ERR_INVALID_ARG;
        char pair[3]={hex[i*2],hex[i*2+1],0};
        out[i]=(uint8_t)strtoul(pair,NULL,16);
        if (!out[i]) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t scan(const char *ssid_hex)
{
    uint8_t ssid[33]={0};
    esp_err_t error=decode(ssid_hex,ssid,1,32);
    if (error==ESP_OK) error=start_wifi();
    wifi_ap_record_t aps[8]={0};
    uint16_t count=8,total=0;
    if (error==ESP_OK) {
        wifi_scan_config_t config={.ssid=ssid,.show_hidden=true};
        error=esp_wifi_scan_start(&config,true);
    }
    if (error==ESP_OK) error=esp_wifi_scan_get_ap_num(&total);
    if (error==ESP_OK) error=esp_wifi_scan_get_ap_records(&count,aps);
    unsigned matches=0;
    if (error==ESP_OK) for (unsigned i=0;i<count;++i) {
        if (strnlen((const char *)aps[i].ssid,33)==strlen((char *)ssid) &&
                !memcmp(aps[i].ssid,ssid,strlen((char *)ssid))) {
            ++matches;
            printf("MEO:NET_AP rssi=%d channel=%u auth=%u\n",aps[i].rssi,aps[i].primary,aps[i].authmode);
        }
    }
    printf("MEO:NET_SCAN error=0x%x total=%u matches=%u source=C5_RADIO\n",(unsigned)error,total,matches);
    mbedtls_platform_zeroize(ssid,sizeof(ssid));
    return error;
}

static esp_err_t join(const char *ssid_hex, const char *password_hex)
{
    wifi_config_t config={0};
    esp_err_t error=decode(ssid_hex,config.sta.ssid,1,32);
    if (error==ESP_OK) error=decode(password_hex,config.sta.password,8,63);
    if (error==ESP_OK) error=start_wifi();
    if (error==ESP_OK && (xEventGroupGetBits(events)&NET_UP)) error=ESP_ERR_INVALID_STATE;
    if (error==ESP_OK) {
        config.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
        config.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
        error=esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (error==ESP_OK) error=esp_wifi_set_config(WIFI_IF_STA,&config);
    }
    mbedtls_platform_zeroize(&config,sizeof(config));
    if (error==ESP_OK) {
        disconnect_reason=0;
        xEventGroupClearBits(events,NET_UP|NET_DOWN);
        error=esp_wifi_connect();
    }
    if (error==ESP_OK) {
        EventBits_t bits=xEventGroupWaitBits(events,NET_UP|NET_DOWN,pdFALSE,pdFALSE,pdMS_TO_TICKS(35000));
        if (!(bits&NET_UP)) error=(bits&NET_DOWN) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    esp_netif_ip_info_t ip={0};
    wifi_ap_record_t ap={0};
    if (error==ESP_OK) error=esp_netif_get_ip_info(sta,&ip);
    if (error==ESP_OK && !ip.ip.addr) error=ESP_ERR_INVALID_RESPONSE;
    if (error==ESP_OK) error=esp_wifi_sta_get_ap_info(&ap);
    printf("MEO:NET_JOIN error=0x%x reason=%u storage=RAM source=LIVE_IP_EVENT\n",(unsigned)error,disconnect_reason);
    if (error==ESP_OK) {
        printf("MEO:NET_IP ip=" IPSTR " gateway=" IPSTR " netmask=" IPSTR " rssi=%d channel=%u auth=%u\n",
               IP2STR(&ip.ip),IP2STR(&ip.gw),IP2STR(&ip.netmask),ap.rssi,ap.primary,ap.authmode);
    }
    return error;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        size_t *bytes=event->user_data;
        *bytes+=(size_t)event->data_len;
    }
    return ESP_OK;
}

static esp_err_t probe(const char *epoch_text, const char *target_text)
{
    if (!started || !(xEventGroupGetBits(events)&NET_UP) || !epoch_text || !target_text) return ESP_ERR_INVALID_STATE;
    char *end=NULL;
    uint64_t epoch=strtoull(epoch_text,&end,10);
    if (!*epoch_text || *end || epoch<1704067200ULL || epoch>4102444800ULL) return ESP_ERR_INVALID_ARG;
    bool example=!strcmp(target_text,"example"), espressif=!strcmp(target_text,"espressif");
    if (!example && !espressif) return ESP_ERR_INVALID_ARG;
    const char *host=example ? "example.com" : "www.espressif.com";
    const char *url=example ? "https://example.com/" : "https://www.espressif.com/robots.txt";
    struct timeval now={.tv_sec=(time_t)epoch};
    if (settimeofday(&now,NULL)) return ESP_FAIL;
    printf("MEO:NET_TIME epoch=%" PRIu64 " source=PC_UTC cert_time_check=1\n",epoch);
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*answer=NULL;
    int dns=getaddrinfo(host,"443",&hints,&answer);
    char address[INET_ADDRSTRLEN]="none";
    if (!dns && answer) {
        struct sockaddr_in *a=(struct sockaddr_in *)answer->ai_addr;
        (void)inet_ntop(AF_INET,&a->sin_addr,address,sizeof(address));
    }
    printf("MEO:NET_DNS target=%s error=%d address=%s source=P4_LWIP\n",target_text,dns,address);
    if (answer) freeaddrinfo(answer);
    if (dns || !strcmp(address,"none")) return ESP_ERR_NOT_FOUND;
    size_t body_bytes=0;
    esp_http_client_config_t config={.url=url,.timeout_ms=15000,.crt_bundle_attach=esp_crt_bundle_attach,
        .disable_auto_redirect=true,.event_handler=http_event,.user_data=&body_bytes,.buffer_size=2048};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    int64_t start=esp_timer_get_time();
    esp_err_t error=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);
    bool complete=esp_http_client_is_complete_data_received(client);
    bool passed=error==ESP_OK && status>=200 && status<300 && complete && body_bytes>0;
    printf("MEO:NET_HTTPS target=%s error=0x%x status=%d body_bytes=%u complete=%u cert_verified=%u elapsed_ms=%" PRId64 "\n",
           target_text,(unsigned)error,status,(unsigned)body_bytes,complete,error==ESP_OK,(esp_timer_get_time()-start)/1000);
    (void)esp_http_client_cleanup(client);
    return passed ? ESP_OK : error!=ESP_OK ? error : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t stop(void)
{
    if (!initialized) {
        printf("MEO:NET_STOP cleared=1 stopped=1 deinitialized=1\n");
        return ESP_OK;
    }
    if (started) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    wifi_config_t empty={0};
    esp_err_t clear=esp_wifi_set_config(WIFI_IF_STA,&empty);
    esp_err_t halted=started ? esp_wifi_stop() : ESP_OK;
    if (halted==ESP_OK) started=false;
    esp_err_t deinit=esp_wifi_deinit();
    if (deinit==ESP_OK) { initialized=false; started=false; }
    xEventGroupClearBits(events,NET_UP|NET_DOWN);
    printf("MEO:NET_STOP cleared=%u stopped=%u deinitialized=%u\n",clear==ESP_OK,halted==ESP_OK,deinit==ESP_OK);
    return clear!=ESP_OK ? clear : halted!=ESP_OK ? halted : deinit;
}

esp_err_t wifi_diagnostic_command(char *line)
{
    char *save=NULL,*op=strtok_r(line," ",&save),*a=strtok_r(NULL," ",&save),
         *b=strtok_r(NULL," ",&save),*extra=strtok_r(NULL," ",&save);
    if (!op || extra) return ESP_ERR_INVALID_ARG;
    if (!strcmp(op,"NET_SCAN") && a && !b) return scan(a);
    if (!strcmp(op,"NET_JOIN") && a && b) return join(a,b);
    if (!strcmp(op,"NET_PROBE") && a && b) return probe(a,b);
    if (!strcmp(op,"NET_STOP") && !a) return stop();
    return ESP_ERR_INVALID_ARG;
}
