#pragma once
#include <stdbool.h>
/* Commands only schedule work. SDIO is owned by this profile until restart. */
bool bsp_c5_loader_command(const char *line);
void bsp_c5_loader_service(void);
