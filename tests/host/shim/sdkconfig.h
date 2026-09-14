/* Host stand-in for the generated sdkconfig.h. Only the options app_config's defaults read, with
 * the same values as main/Kconfig.projbuild, so the tests exercise the shipped defaults. */
#pragma once

#define CONFIG_GW_DALI_TX_GPIO 14
#define CONFIG_GW_DALI_RX_GPIO 5
#define CONFIG_GW_LED_GPIO 8
#define CONFIG_GW_AP_PASSWORD "dali12345"
