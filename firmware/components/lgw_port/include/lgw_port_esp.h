/* ESP-IDF set-up for the libloragw platform layer. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sclk_gpio;
    int mosi_gpio;
    int miso_gpio;
    int cs_gpio;
    int clock_hz;
} lgw_port_spi_config_t;

/**
 * Initialise the concentrator SPI bus. This is the only owner of the bus.
 * No DMA: transfers run as polled chunks of SOC_SPI_MAXIMUM_BUFFER_SIZE with
 * CS held, so nothing is allocated per transfer.
 */
esp_err_t lgw_port_spi_init(const lgw_port_spi_config_t *cfg);

/** Set the temperature used when no STTS751 is reachable (default 30 C). */
void lgw_port_set_fixed_temperature(float celsius);

#ifdef __cplusplus
}
#endif
