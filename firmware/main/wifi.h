/* Wi-Fi station through the on-module ESP32-C6 (ESP-Hosted over SDIO). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** NVS namespace and keys written by tools/provision_wifi.py. */
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

#define WIFI_SETUP_MINUTES 15
#define WIFI_SETUP_MAX_NETS 16

/**
 * Start Wi-Fi. With credentials in NVS: connect, and reconnect with backoff.
 * Without: scan, then open the setup access point "lorawan-gateway-XXXXXX"
 * (open, 192.168.4.1) for WIFI_SETUP_MINUTES; the web UI's setup page
 * stores credentials (ADR-0011). A power cycle opens it again.
 * Needs nvs_flash_init(), esp_netif_init() and the default event loop.
 */
esp_err_t wifi_start(void);

bool wifi_has_ip(void);

/** Store credentials in NVS (SSID 1..32 bytes, WPA2 password 8..63). */
esp_err_t wifi_store_credentials(const char *ssid, const char *pass);

/** True while the setup access point is open. */
bool wifi_setup_active(void);

/** SSIDs seen by the scan before the setup AP opened (NUL-terminated). */
size_t wifi_setup_networks(char names[][33], size_t max);
