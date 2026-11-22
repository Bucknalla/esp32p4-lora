/* Board layer: pins and probes for the Waveshare ESP32-P4 dev kit + Nebra HAT. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool ecc608_present; /**< secure element answered at CONFIG_GW_ECC608_ADDR */
    int i2c_devices;     /**< number of addresses that ACKed on the header I2C bus */
} board_probe_t;

/** Log chip revision, cores, flash, PSRAM, reset reason and app version. */
void board_log_identity(void);

/**
 * Scan the header I2C bus (GPIO7/8). Wakes an ECC608 first: it does not ACK
 * its address while asleep. Read-only; the bus is released afterwards.
 */
esp_err_t board_probe_i2c(board_probe_t *out);
