/* LoRa Basics Station LNS client (ADR-0016): the TTN link over a TLS WebSocket
 * (TTN and hybrid modes). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Start the client task: forwards radio_pkt_t items from `rx_queue` (the
 *  radio's queue in TTN mode, the local server's leftovers in hybrid mode) and
 *  sends the server's downlinks to the radio. */
esp_err_t lbsfwd_start(QueueHandle_t rx_queue);

/** True while the WebSocket is open and router_config has arrived. */
bool lbsfwd_link_up(void);

/** One status line (console STATUS); empty when the client does not run. */
void lbsfwd_status(char *out, size_t cap);
