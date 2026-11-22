#include "conc.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lgw_port.h"
#include "lgw_port_esp.h"
#include "sdkconfig.h"

static const char *TAG = "conc";

#define CONC_RESET_PULSE_MS 100
#define CONC_RESET_SETTLE_MS 100
#define SURVEY_SETTLE_MS 20

/* SX1302: 5-byte read frame [mux, R|addr_hi, addr_lo, dummy, data]
 * (sx1302_hal loragw_spi.c). Version register COMMON_VERSION at 0x5606. */
#define SX1302_MUX_TARGET 0x00U
#define SX1302_REG_VERSION 0x5606U
/* SX1301: 2-byte read frame [R|addr, data]. Version register 0x01 = 103. */
#define SX1301_REG_VERSION 0x01U
#define SX1301_VERSION 0x67U

static bool s_spi_ready;

esp_err_t conc_spi_init(void)
{
    const lgw_port_spi_config_t cfg = {
        .sclk_gpio = CONFIG_GW_SPI_SCLK_GPIO,
        .mosi_gpio = CONFIG_GW_SPI_MOSI_GPIO,
        .miso_gpio = CONFIG_GW_SPI_MISO_GPIO,
        .cs_gpio = CONFIG_GW_SPI_CS_GPIO,
        .clock_hz = CONFIG_GW_SPI_CLOCK_HZ,
    };
    ESP_RETURN_ON_ERROR(lgw_port_spi_init(&cfg), TAG, "SPI init");
    s_spi_ready = true;
    return ESP_OK;
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    const lgw_port_spi_seg_t seg = {.tx = tx, .rx = rx, .len = len};
    return (lgw_port_spi_message(&seg, 1) == (int)len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t sx1302_read(uint16_t addr, uint8_t *val)
{
    uint8_t tx[5] = {SX1302_MUX_TARGET, (uint8_t)((addr >> 8) & 0x7FU), (uint8_t)(addr & 0xFFU),
                     0x00, 0x00};
    uint8_t rx[5] = {0};
    ESP_RETURN_ON_ERROR(xfer(tx, rx, sizeof tx), TAG, "SX1302 read");
    ESP_LOGI(TAG, "SX1302 frame rx: %02x %02x %02x %02x %02x", rx[0], rx[1], rx[2], rx[3], rx[4]);
    *val = rx[4];
    return ESP_OK;
}

static esp_err_t sx1301_read(uint8_t addr, uint8_t *val)
{
    uint8_t tx[2] = {(uint8_t)(addr & 0x7FU), 0x00};
    uint8_t rx[2] = {0};
    ESP_RETURN_ON_ERROR(xfer(tx, rx, sizeof tx), TAG, "SX1301 read");
    ESP_LOGI(TAG, "SX1301 frame rx: %02x %02x", rx[0], rx[1]);
    *val = rx[1];
    return ESP_OK;
}

/* An SX1302/3 reports version 0x1x. 0x00 and 0xFF mean no answer. */
static int sx1302_version_plausible(uint8_t v)
{
    return (v & 0xF0U) == 0x10U;
}

esp_err_t conc_identify(conc_id_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL && s_spi_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    memset(out, 0, sizeof(*out));

    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(sx1302_read(SX1302_REG_VERSION, &v), TAG, "SX1302 version");
    if (sx1302_version_plausible(v)) {
        out->chip = CONC_CHIP_SX1302;
        out->version = v;
        ESP_LOGI(TAG, "concentrator: SX1302/SX1303, version 0x%02x", v);
        return ESP_OK;
    }

    /* Only try the SX1301 frame when the SX1302 frame got no valid answer. */
    ESP_RETURN_ON_ERROR(sx1301_read(SX1301_REG_VERSION, &v), TAG, "SX1301 version");
    if (v == SX1301_VERSION) {
        out->chip = CONC_CHIP_SX1301;
        out->version = v;
        /* Port check: header and data as two segments. Only answers 0x67 if
         * CS stays low across segments, as upstream burst access needs. */
        const uint8_t hdr = SX1301_REG_VERSION;
        uint8_t data = 0;
        const lgw_port_spi_seg_t segs[2] = {
            {.tx = &hdr, .rx = NULL, .len = 1},
            {.tx = NULL, .rx = &data, .len = 1},
        };
        const int n = lgw_port_spi_message(segs, 2);
        ESP_LOGI(TAG, "2-segment read (CS held): %d bytes, value 0x%02x -> %s", n, data,
                 (n == 2 && data == SX1301_VERSION) ? "OK" : "FAIL");
        ESP_LOGI(TAG, "concentrator: SX1301 (version 0x%02x)", v);
        return ESP_OK;
    }

    out->version = v;
    ESP_LOGW(TAG, "concentrator: no valid answer (last byte 0x%02x)", v);
    return ESP_ERR_NOT_FOUND;
}

/* Read a pin with pull-up, then pull-down. Same value both times = driven
 * (or strongly pulled) externally; follows the pull = floating. Input only. */
static void survey_pin(int pin, const char *name)
{
    const gpio_num_t g = (gpio_num_t)pin;
    if (gpio_reset_pin(g) != ESP_OK || gpio_set_direction(g, GPIO_MODE_INPUT) != ESP_OK) {
        ESP_LOGW(TAG, "survey GPIO%d: config failed", pin);
        return;
    }
    /* 20 ms settle: a ~45 kOhm internal pull needs ms to move an RC node. */
    (void)gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(SURVEY_SETTLE_MS));
    const int up = gpio_get_level(g);
    (void)gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(SURVEY_SETTLE_MS));
    const int down = gpio_get_level(g);
    (void)gpio_set_pull_mode(g, GPIO_FLOATING);
    ESP_LOGI(TAG, "survey GPIO%-2d %-22s pu=%d pd=%d -> %s", pin, name, up, down,
             up == down ? (up ? "held HIGH" : "held LOW") : "floating");
}

void conc_pin_survey(void)
{
    survey_pin(6, "reset (Pi 15)");
    survey_pin(2, "SPI0 MISO (Pi 21)");
    /* Helium Hotspot HAT V1 JP1 band jumpers: a fitted jumper shorts to GND. */
    survey_pin(26, "JP1 FR868 (Pi 31)");
    survey_pin(48, "JP1 FR915 (Pi 33)");
    survey_pin(33, "JP1 FR470 (Pi 29)");
    survey_pin(54, "JP1 FR433 (Pi 32)");
}

esp_err_t conc_reset(void)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_GW_CONC_RESET_GPIO;
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "reset low");
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "reset gpio config");
    /* Weakest drive: limits current if something else drives this line. */
    ESP_RETURN_ON_ERROR(gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_0), TAG, "drive cap");
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "reset high");
    esp_rom_delay_us(1000);
    if (gpio_get_level(pin) != 1) {
        /* Another driver holds the line low: release it and stop. */
        (void)gpio_set_direction(pin, GPIO_MODE_INPUT);
        ESP_LOGE(TAG, "reset GPIO%d did not go high: line is driven; released", (int)pin);
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(CONC_RESET_PULSE_MS));
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(CONC_RESET_SETTLE_MS));
    ESP_LOGI(TAG, "reset pulse on GPIO%d done (read-back OK)", (int)pin);
    return ESP_OK;
}
