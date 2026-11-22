/*
 * SPI for libloragw on ESP-IDF.
 *
 * The bus runs without DMA. On the ESP32-P4, a DMA RX buffer must have a
 * cache-aligned address and length, otherwise spi_master allocates a bounce
 * buffer on every transfer. Polled chunks of SOC_SPI_MAXIMUM_BUFFER_SIZE
 * bytes, chained with SPI_TRANS_CS_KEEP_ACTIVE, need no allocation and keep CS
 * low for the whole message, as Linux spidev does.
 */
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lgw_port.h"
#include "lgw_port_esp.h"
#include "lgw_port_spidev.h"
#include "soc/soc_caps.h"

static const char *TAG = "lgw_spi";

#define LGW_PORT_SPI_HOST SPI2_HOST
#define LGW_PORT_SPI_FD 3
#define LGW_PORT_CHUNK SOC_SPI_MAXIMUM_BUFFER_SIZE
#define LGW_PORT_MAX_SEGS 4

static spi_device_handle_t s_dev;
static int s_clock_hz;
static bool s_open;
static const uint8_t s_zeros[LGW_PORT_CHUNK];

esp_err_t lgw_port_spi_init(const lgw_port_spi_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg");
    ESP_RETURN_ON_FALSE(s_dev == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const spi_bus_config_t bus = {
        .sclk_io_num = cfg->sclk_gpio,
        .mosi_io_num = cfg->mosi_gpio,
        .miso_io_num = cfg->miso_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LGW_PORT_CHUNK,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LGW_PORT_SPI_HOST, &bus, SPI_DMA_DISABLED), TAG,
                        "bus init");

    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = cfg->clock_hz,
        .spics_io_num = cfg->cs_gpio,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(LGW_PORT_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        (void)spi_bus_free(LGW_PORT_SPI_HOST);
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return err;
    }
    s_clock_hz = cfg->clock_hz;
    ESP_LOGI(TAG, "SCLK %d MOSI %d MISO %d CS %d, %d Hz, no DMA, %d-byte chunks",
             cfg->sclk_gpio, cfg->mosi_gpio, cfg->miso_gpio, cfg->cs_gpio, cfg->clock_hz,
             LGW_PORT_CHUNK);
    return ESP_OK;
}

int lgw_port_spi_message(const lgw_port_spi_seg_t *segs, size_t n_segs)
{
    if (s_dev == NULL || segs == NULL || n_segs == 0) {
        return -1;
    }

    /* Index of the last chunk: the only one sent without CS_KEEP_ACTIVE. */
    size_t last_seg = n_segs;
    while (last_seg > 0 && segs[last_seg - 1].len == 0) {
        last_seg--;
    }
    if (last_seg == 0) {
        return 0;
    }
    last_seg--;

    /* IDF accepts only portMAX_DELAY here. The port is the bus's only user,
     * so the acquire cannot wait on another device. */
    if (spi_device_acquire_bus(s_dev, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "bus acquire failed");
        return -1;
    }

    int total = 0;
    esp_err_t err = ESP_OK;
    for (size_t s = 0; s <= last_seg && err == ESP_OK; s++) {
        const lgw_port_spi_seg_t *seg = &segs[s];
        for (size_t off = 0; off < seg->len && err == ESP_OK; off += LGW_PORT_CHUNK) {
            const size_t n = (seg->len - off < LGW_PORT_CHUNK) ? seg->len - off : LGW_PORT_CHUNK;
            const bool final = (s == last_seg) && (off + n == seg->len);
            spi_transaction_t t = {
                .flags = final ? 0 : SPI_TRANS_CS_KEEP_ACTIVE,
                .length = n * 8U,
                .tx_buffer = seg->tx ? seg->tx + off : s_zeros,
                .rx_buffer = seg->rx ? seg->rx + off : NULL,
            };
            err = spi_device_polling_transmit(s_dev, &t);
            if (err == ESP_OK) {
                total += (int)n;
            }
        }
    }
    spi_device_release_bus(s_dev);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transfer failed: %s", esp_err_to_name(err));
        return -1;
    }
    return total;
}

int lgw_port_spi_open(const char *path, int flags)
{
    (void)flags;
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "open %s: SPI not initialised", path ? path : "(null)");
        return -1;
    }
    if (s_open) {
        ESP_LOGE(TAG, "open %s: already open", path ? path : "(null)");
        return -1;
    }
    s_open = true;
    return LGW_PORT_SPI_FD;
}

int lgw_port_spi_close(int fd)
{
    if (fd != LGW_PORT_SPI_FD || !s_open) {
        return -1;
    }
    s_open = false;
    return 0;
}

static int spi_message(const struct spi_ioc_transfer *k, size_t n)
{
    if (n == 0 || n > LGW_PORT_MAX_SEGS) {
        return -1;
    }
    lgw_port_spi_seg_t segs[LGW_PORT_MAX_SEGS];
    for (size_t i = 0; i < n; i++) {
        segs[i].tx = (const uint8_t *)(uintptr_t)k[i].tx_buf;
        segs[i].rx = (uint8_t *)(uintptr_t)k[i].rx_buf;
        segs[i].len = k[i].len;
    }
    return lgw_port_spi_message(segs, n);
}

int lgw_port_spi_ioctl(int fd, unsigned long request, void *arg)
{
    if (fd != LGW_PORT_SPI_FD || !s_open || arg == NULL) {
        return -1;
    }
    int *val = (int *)arg;

    switch (request) {
    case SPI_IOC_WR_MODE:
        return (*val == SPI_MODE_0) ? 0 : -1;
    case SPI_IOC_RD_MODE:
        *val = SPI_MODE_0;
        return 0;
    case SPI_IOC_WR_LSB_FIRST:
        return (*val == 0) ? 0 : -1;
    case SPI_IOC_RD_LSB_FIRST:
        *val = 0;
        return 0;
    case SPI_IOC_WR_BITS_PER_WORD:
        return (*val == 0 || *val == 8) ? 0 : -1;
    case SPI_IOC_RD_BITS_PER_WORD:
        *val = 8;
        return 0;
    case SPI_IOC_WR_MAX_SPEED_HZ:
        /* The clock is fixed at init from Kconfig; upstream asks for 2 MHz. */
        if (*val != s_clock_hz) {
            ESP_LOGW(TAG, "HAL asks for %d Hz, bus runs at %d Hz", *val, s_clock_hz);
        }
        return 0;
    case SPI_IOC_RD_MAX_SPEED_HZ:
        *val = s_clock_hz;
        return 0;
    default:
        break;
    }

    if (request > LGW_PORT_SPI_IOC_MESSAGE_BASE &&
        request <= LGW_PORT_SPI_IOC_MESSAGE_BASE + LGW_PORT_MAX_SEGS) {
        return spi_message((const struct spi_ioc_transfer *)arg,
                           (size_t)(request - LGW_PORT_SPI_IOC_MESSAGE_BASE));
    }
    ESP_LOGE(TAG, "unsupported ioctl 0x%lx", request);
    return -1;
}
