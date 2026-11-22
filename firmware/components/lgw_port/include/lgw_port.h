/*
 * Platform layer under Semtech libloragw (sx1302_hal).
 *
 * This header is included by patched upstream files, so it uses only standard
 * C types. ESP-IDF specific set-up lives in lgw_port_esp.h.
 */
#ifndef LGW_PORT_H
#define LGW_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Busy-wait below one RTOS tick, otherwise sleep. Never returns early. */
void lgw_port_wait_us(unsigned long delay_us);
void lgw_port_wait_ms(unsigned long delay_ms);

/** Temperature (deg C) reported when the card has no reachable STTS751. */
float lgw_port_fixed_temperature(void);

/** One segment of an SPI message. tx NULL sends zeros; rx NULL discards. */
typedef struct {
    const uint8_t *tx;
    uint8_t *rx;
    size_t len;
} lgw_port_spi_seg_t;

/**
 * Run segments back to back with chip select held for the whole message.
 * Returns the number of bytes transferred, or -1 on error.
 */
int lgw_port_spi_message(const lgw_port_spi_seg_t *segs, size_t n_segs);

#ifdef __cplusplus
}
#endif

#endif /* LGW_PORT_H */
