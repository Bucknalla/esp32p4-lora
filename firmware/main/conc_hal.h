/* Concentrator task: libloragw on core 1, packets to the forwarder queue. */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Start the radio task. SX1301 builds queue radio_pkt_t items to `rx_queue`;
 *  SX1302 builds run a one-shot HAL check only. */
esp_err_t conc_hal_start(QueueHandle_t rx_queue);

/* EU868 radio centre frequencies (the Nebra/TTN plan: radio 0 for 867.1-867.9,
 * radio 1 for 868.1-868.5 MHz); the Basics Station client checks the server's
 * router_config against them. */
#define CONC_EU868_RADIO0_HZ 867500000U
#define CONC_EU868_RADIO1_HZ 868500000U
