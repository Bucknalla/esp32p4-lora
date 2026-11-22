/*
 * Service console on the USB serial port (UART0, the log console).
 * Line protocol: core/include/lwc/console_cmd.h. Used by
 * tools/provision_wifi.py; the board writes credentials to NVS itself (works
 * unchanged if NVS encryption is turned on later, ADR-0006).
 */
#pragma once

#include "esp_err.h"

/** Install the UART driver on the console UART and start the reader task. */
esp_err_t console_start(void);
