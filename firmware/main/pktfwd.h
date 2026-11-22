/* Semtech UDP packet forwarder client (core 0). */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Start the forwarder task. It waits for an IP (Wi-Fi or Ethernet), starts
 *  SNTP, resolves the server and forwards radio_pkt_t items from `rx_queue`. */
esp_err_t pktfwd_start(QueueHandle_t rx_queue);
