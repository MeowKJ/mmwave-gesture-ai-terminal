#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ESP-Hosted Wi-Fi over the module's internal SDIO bus.
 *
 * The transport is brought up in the background so a missing or unflashed C5
 * can never stall display/USB startup. Credentials live in NVS under "wifi";
 * the last successful SSID is re-joined on boot. Everything here is
 * non-blocking: requests are queued and serviced from the app task. */
typedef enum {
    BSP_WIFI_OFF = 0,     /* transport not up yet */
    BSP_WIFI_STARTING,    /* Hosted bring-up in progress */
    BSP_WIFI_READY,       /* radio up, not associated */
    BSP_WIFI_SCANNING,
    BSP_WIFI_JOINING,
    BSP_WIFI_ONLINE,      /* associated + IP */
    BSP_WIFI_ERROR,
} bsp_wifi_state_t;

typedef struct {
    bsp_wifi_state_t state;
    int error;            /* esp_err_t, 0 when the last step succeeded */
    char ssid[33];        /* current or last target SSID */
    int rssi;             /* connected signal, 0 when unknown */
    char ip[16];          /* dotted quad, "" when not online */
    unsigned ap_count;    /* cached scan results */
    unsigned ap_raw;      /* driver-reported count of the last scan */
} bsp_wifi_status_t;

void bsp_wifi_hosted_init(void);
void bsp_wifi_hosted_service(void);
bool bsp_wifi_hosted_command(const char *line);

bsp_wifi_status_t bsp_wifi_hosted_status(void);
/* Queued requests; false when the radio is not ready for them yet. */
bool bsp_wifi_scan(void);
bool bsp_wifi_join_saved(const char *ssid);
bool bsp_wifi_set_credentials(const char *ssid, const char *pass);
/* Copy up to max cached scan results; returns how many were copied. */
unsigned bsp_wifi_ap_list(char ssid[][33], int rssi[], unsigned max);
