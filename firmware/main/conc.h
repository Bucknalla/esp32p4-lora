/* Concentrator SPI link and identification (pre-HAL bring-up). */
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CONC_CHIP_UNKNOWN = 0,
    CONC_CHIP_SX1302, /**< SX1302 or SX1303 (same version register) */
    CONC_CHIP_SX1301,
} conc_chip_t;

typedef struct {
    conc_chip_t chip;
    uint8_t version; /**< raw version register value */
} conc_id_t;

/** Initialise the SPI bus and device for the concentrator. */
esp_err_t conc_spi_init(void);

/**
 * Identify the concentrator with read-only register accesses.
 * Does not touch the reset pin.
 */
esp_err_t conc_identify(conc_id_t *out);

/** Bring-up diagnostic: log whether HAT control pins are driven or floating.
 *  Input-only; call before conc_spi_init(). */
void conc_pin_survey(void);

/** Pulse the concentrator reset pin: high ~100 ms, then low, ~100 ms settle. */
esp_err_t conc_reset(void);
