/* BLE Wi-Fi provisioning (ADR-0014). */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define BLE_PROV_MINUTES 15

/**
 * Start the provisioning task. With no Wi-Fi credentials, BLE setup opens at
 * once for BLE_PROV_MINUTES. Call after wifi_start(). A C6 without the BLE
 * controller only logs an error: the gateway runs on without BLE setup.
 */
esp_err_t ble_prov_start(void);

/** Open BLE setup for BLE_PROV_MINUTES (the HAT button). Safe from any task. */
void ble_prov_open(const char *why);

/** True while the gateway advertises for setup. */
bool ble_prov_active(void);

/** "PROV_XXXXXX": the name the ESP BLE Provisioning app lists. */
const char *ble_prov_name(void);
