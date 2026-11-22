/* Semtech UDP packet forwarder client (core 0). */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Start the forwarder task (TTN and hybrid modes; nothing in standalone). It
 *  waits for an IP (Wi-Fi or Ethernet), starts SNTP, resolves the server and
 *  forwards radio_pkt_t items from `rx_queue` (the radio's queue in TTN mode,
 *  the local server's leftovers in hybrid mode). */
esp_err_t pktfwd_start(QueueHandle_t rx_queue);

/** True while the server answers PULL_DATA (any task may call). */
bool pktfwd_link_up(void);

/** Gateway EUI as 16 upper-case hex digits (from the base MAC). */
esp_err_t pktfwd_eui(char out[17]);
