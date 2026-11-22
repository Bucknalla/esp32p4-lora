/* Concentrator task: libloragw on core 1, packets to the forwarder queue. */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Start the radio task. SX1301 builds queue radio_pkt_t items to `rx_queue`;
 *  SX1302 builds run a one-shot HAL check only. */
esp_err_t conc_hal_start(QueueHandle_t rx_queue);
