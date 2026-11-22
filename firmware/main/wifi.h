/* Wi-Fi station through the on-module ESP32-C6 (ESP-Hosted over SDIO). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

/** NVS namespace and keys written by tools/provision_wifi.py. */
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASS "pass"

/**
 * Start the station. With credentials in NVS: connect, and reconnect with
 * backoff. Without: the station stays idle until BLE provisioning
 * (ble_prov.c, ADR-0014) applies a network.
 * Needs nvs_flash_init(), esp_netif_init() and the default event loop.
 */
esp_err_t wifi_start(void);

bool wifi_has_ip(void);

/** True once a network is set (from NVS at start, or applied). */
bool wifi_configured(void);

/** Store credentials in NVS (SSID 1..32 bytes, WPA2 password 8..63). */
esp_err_t wifi_store_credentials(const char *ssid, const char *pass);

/** Store credentials and join that network now (BLE provisioning). */
esp_err_t wifi_apply_credentials(const char *ssid, const char *pass);

typedef enum {
    WIFI_LINK_UNCONFIGURED,
    WIFI_LINK_CONNECTING,
    WIFI_LINK_UP,
    WIFI_LINK_AUTH_FAILED, /* last attempt: wrong password or handshake failure */
    WIFI_LINK_NOT_FOUND,   /* last attempt: no AP with that name (and security) */
} wifi_link_t;

wifi_link_t wifi_link(void);

/** Blocking scan; *n is the capacity in, the record count out. */
esp_err_t wifi_scan(wifi_ap_record_t *aps, uint16_t *n);
