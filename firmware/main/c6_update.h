/* Keeps the ESP32-C6 co-processor firmware in step with the host's ESP-Hosted. */
#pragma once

#include "esp_err.h"

/**
 * Compare the C6's ESP-Hosted version with the host's. If major.minor differ
 * and the `cp_fw` partition holds a C6 image, push it to the C6 and restart.
 * At most C6_UPDATE_MAX_TRIES attempts (NVS counter), so a bad image cannot
 * cause a reboot loop. Call before wifi_start(). Needs nvs_flash_init().
 * Returns ESP_OK when no update was needed or it was skipped.
 */
esp_err_t c6_update_check(void);
