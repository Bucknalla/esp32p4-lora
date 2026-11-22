/* Wi-Fi station through the on-module ESP32-C6 (ESP-Hosted over SDIO). */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** NVS namespace and keys written by tools/provision_wifi.py. */
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

/**
 * Start Wi-Fi. With credentials in NVS: connect, and reconnect with backoff.
 * Without: log a scan of nearby networks (proves the C6 link) and stay idle.
 * Needs nvs_flash_init(), esp_netif_init() and the default event loop.
 */
esp_err_t wifi_start(void);

bool wifi_has_ip(void);
