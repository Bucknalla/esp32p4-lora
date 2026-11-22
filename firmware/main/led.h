/* Status LED on the HAT (Nebra V1: the LED1 anode on net STATUS, P4 GPIO4).
 * Fast blink: no network yet. Slow blink: setup access point open. Steady:
 * online. Double blink: needs attention (upstream down, radio restarting or
 * missing, image pending verify). A downlink inverts it for 150 ms. */
#pragma once

#include "esp_err.h"

esp_err_t led_start(void);

/** Current state name: boot, setup, online or fault. */
const char *led_state(void);
