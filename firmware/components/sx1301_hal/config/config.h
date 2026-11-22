/* Replaces the lora_gateway Makefile's generated config.h (library.cfg). */
#ifndef _LORAGW_CONFIGURATION_H
#define _LORAGW_CONFIGURATION_H

#include "sdkconfig.h"

#define LIBLORAGW_VERSION "5.0.1"

/* Bring-up: lgw_start reports its errors only through DEBUG_* macros. */
#ifdef CONFIG_GW_LGW_DEBUG
#define DEBUG_HAL 1
#define DEBUG_REG 1
#else
#define DEBUG_HAL 0
#define DEBUG_REG 0
#endif
#define DEBUG_AUX 0
#define DEBUG_SPI 0
#define DEBUG_GPS 0
#define DEBUG_GPIO 0
#define DEBUG_LBT 0

#endif
