/*
 * Minimal stand-in for <linux/spi/spidev.h> used by the patched libloragw SPI
 * files. Upstream keeps building its frames in struct spi_ioc_transfer; the
 * port executes them with lgw_port_spi_ioctl() instead of the Linux ioctl().
 */
#ifndef LGW_PORT_SPIDEV_H
#define LGW_PORT_SPIDEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_MODE_0 0

enum {
    SPI_IOC_WR_MODE = 1,
    SPI_IOC_RD_MODE,
    SPI_IOC_WR_LSB_FIRST,
    SPI_IOC_RD_LSB_FIRST,
    SPI_IOC_WR_BITS_PER_WORD,
    SPI_IOC_RD_BITS_PER_WORD,
    SPI_IOC_WR_MAX_SPEED_HZ,
    SPI_IOC_RD_MAX_SPEED_HZ,
};

#define LGW_PORT_SPI_IOC_MESSAGE_BASE 0x100UL
#define SPI_IOC_MESSAGE(n) (LGW_PORT_SPI_IOC_MESSAGE_BASE + (unsigned long)(n))

/* Same field names and widths as Linux; only these fields are honoured:
 * tx_buf, rx_buf, len. */
struct spi_ioc_transfer {
    uint64_t tx_buf;
    uint64_t rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word;
    uint8_t cs_change;
    uint8_t tx_nbits;
    uint8_t rx_nbits;
    uint8_t word_delay_usecs;
    uint8_t pad;
};

int lgw_port_spi_open(const char *path, int flags);
int lgw_port_spi_close(int fd);
int lgw_port_spi_ioctl(int fd, unsigned long request, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LGW_PORT_SPIDEV_H */
