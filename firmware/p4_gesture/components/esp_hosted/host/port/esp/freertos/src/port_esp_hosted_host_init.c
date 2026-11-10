/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_hosted.h"

#include "port_esp_hosted_host_log.h"

#include "esp_private/startup_internal.h"

DEFINE_LOG_TAG(host_init);

//ESP_SYSTEM_INIT_FN(esp_hosted_host_init, BIT(0), 120)
static void __attribute__((constructor)) esp_hosted_host_init(void)
{
#if CONFIG_MEOAVE_C5_MAINT_HOST
	/* The headless maintenance application initializes Hosted explicitly after
	 * the scheduler/event loop are running. Keep pre-scheduler startup free of
	 * SDMMC, transport and RPC task creation. Product profiles are unchanged. */
	return;
#else
	ESP_LOGI(TAG, "ESP Hosted : Host chip_ip[%d]", CONFIG_IDF_FIRMWARE_CHIP_ID);
	ESP_ERROR_CHECK(esp_hosted_init());
#endif
}

static void __attribute__((destructor)) esp_hosted_host_deinit(void)
{
	ESP_LOGI(TAG, "ESP Hosted deinit");
	esp_hosted_deinit();
}
