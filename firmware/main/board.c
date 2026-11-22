#include "board.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define I2C_SCAN_HZ 100000U
#define I2C_PROBE_TIMEOUT_MS 20
#define ECC608_WAKE_LOW_US 80U   /* tWLO >= 60 us */
#define ECC608_WAKE_HIGH_US 1600U /* tWHI >= 1500 us */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

void board_log_identity(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_flash_get_size: %s", esp_err_to_name(err));
    }

    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "app %s %s, IDF %s", app->project_name, app->version, app->idf_ver);
    ESP_LOGI(TAG, "chip ESP32-P4 rev v%d.%d, %d cores", chip.revision / 100,
             chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "flash %" PRIu32 " MB, PSRAM %u MB", flash_size / (1024U * 1024U),
             (unsigned)(esp_psram_get_size() / (1024U * 1024U)));
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
#if CONFIG_GW_HAT_NEBRA_HELIUM_V1
    ESP_LOGI(TAG, "HAT config: Nebra Helium Hotspot HAT V1 (SPI0 CE0)");
#elif CONFIG_GW_HAT_NEBRA_HOTSPOT
    ESP_LOGI(TAG, "HAT config: Nebra Light Hotspot carrier (SPI1 CE2)");
#elif CONFIG_GW_HAT_NEBRA_LIGHT_HOTSPOT_II
    ESP_LOGI(TAG, "HAT config: Nebra Light Hotspot II");
#else
    ESP_LOGI(TAG, "HAT config: none");
#endif
}

/* Hold SDA low long enough to wake an ATECC608, then release it. */
static esp_err_t ecc608_wake_pulse(void)
{
    const gpio_num_t sda = (gpio_num_t)CONFIG_GW_I2C_SDA_GPIO;
    ESP_RETURN_ON_ERROR(gpio_set_level(sda, 0), TAG, "SDA level low");
    ESP_RETURN_ON_ERROR(gpio_set_direction(sda, GPIO_MODE_OUTPUT_OD), TAG, "SDA output");
    esp_rom_delay_us(ECC608_WAKE_LOW_US);
    ESP_RETURN_ON_ERROR(gpio_set_level(sda, 1), TAG, "SDA level high");
    ESP_RETURN_ON_ERROR(gpio_set_direction(sda, GPIO_MODE_INPUT), TAG, "SDA input");
    esp_rom_delay_us(ECC608_WAKE_HIGH_US);
    return ESP_OK;
}

esp_err_t board_probe_i2c(board_probe_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->ecc608_present = false;
    out->i2c_devices = 0;

    esp_err_t err = ecc608_wake_pulse();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ECC608 wake pulse failed (%s); scanning anyway", esp_err_to_name(err));
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)CONFIG_GW_I2C_SDA_GPIO,
        .scl_io_num = (gpio_num_t)CONFIG_GW_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            out->i2c_devices++;
            ESP_LOGI(TAG, "I2C device at 0x%02x", addr);
            if (addr == CONFIG_GW_ECC608_ADDR) {
                out->ecc608_present = true;
            }
        }
    }

    err = i2c_del_master_bus(bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_del_master_bus: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "I2C scan: %d device(s); ECC608 at 0x%02x %s", out->i2c_devices,
             CONFIG_GW_ECC608_ADDR, out->ecc608_present ? "FOUND" : "not found");
    return ESP_OK;
}
