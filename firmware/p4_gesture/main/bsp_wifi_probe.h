#pragma once
#include <stdbool.h>

/* Explicit native-USB diagnostic only. No auto probe, reset, flash, CMD0,
 * CMD52 writes, card selection, Hosted handshake or Wi-Fi connection. */
bool bsp_wifi_probe_command(const char *line);
/* Call from app_main, not the TinyUSB callback or LVGL timer. */
void bsp_wifi_probe_service(void);
