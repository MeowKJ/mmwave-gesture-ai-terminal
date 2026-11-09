#pragma once

#include <stdbool.h>

/* Explicitly controlled Hosted bring-up. The default product build does not
 * call this module; the hosted build only starts a worker after a USB command
 * so a missing C5 cannot stall display/USB startup. */
void bsp_wifi_hosted_init(void);
bool bsp_wifi_hosted_command(const char *line);
void bsp_wifi_hosted_service(void);
