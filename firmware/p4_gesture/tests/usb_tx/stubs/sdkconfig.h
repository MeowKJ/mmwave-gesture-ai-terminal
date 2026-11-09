#pragma once

#ifndef CONFIG_APP_USB_HID_ENABLE
#error "Select the enabled or disabled production branch in CMake"
#endif

/* Matches the workspace firmware sdkconfig at test creation. */
#define CONFIG_TINYUSB_CDC_TX_BUFSIZE 512
