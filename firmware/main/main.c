/*
 * Start-up order: identity and boot report, NVS, console, concentrator,
 * C6 check, Wi-Fi, update task, packet
 * forwarder, Ethernet, then a heartbeat.
 */
#include <inttypes.h>

#include "ble_prov.h"
#include "board.h"
#include "gwmode.h"
#include "lns.h"
#include "codecs.h"
#include "mqttc.h"
#include "conc.h"
#include "conc_hal.h"
#include "lbsfwd.h"
#include "radio_pkt.h"
#include "radio_tx.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "eth.h"
#include "c6_update.h"
#include "console.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwc/frame.h"
#include "nvs_flash.h"
#include "ota.h"
#include "fault.h"
#include "logbuf.h"
#include "sv.h"
#include "web.h"
#include "led.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#ifndef CONFIG_GW_OTA_TEST_HANG_BEFORE_START
#define CONFIG_GW_OTA_TEST_HANG_BEFORE_START 0
#endif

static QueueHandle_t s_rx_queue;

#define HEARTBEAT_PERIOD_MS 10000
#define SV_BUDGET_MS 30000

/* With NVS encryption (HMAC scheme, ADR-0006) the first nvs_flash_init burns
 * the HMAC key into eFuse BLOCK_KEY<CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID>. A
 * partition written before encryption was on reads as unusable: erase it. */
static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
        err == ESP_ERR_NVS_CORRUPT_KEY_PART) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#if CONFIG_NVS_ENCRYPTION
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS encrypted (HMAC key in eFuse key block %d)",
                 CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
    }
#endif
    return err;
}

/* Proves the core library links and runs on target. */
static void core_selftest(void)
{
    static const uint8_t frame[] = {0x40, 0x04, 0x03, 0x02, 0x01, 0x80, 0x2A, 0x00,
                                    0x0A, 0xDE, 0xAD, 0x11, 0x22, 0x33, 0x44};
    lwc_frame_t fr;
    lwc_frame_err_t r = lwc_frame_parse(frame, sizeof frame, &fr);
    if (r == LWC_FRAME_OK && fr.u.data.dev_addr == 0x01020304U && fr.u.data.fcnt16 == 42U) {
        ESP_LOGI(TAG, "core self-test: OK");
    } else {
        ESP_LOGE(TAG, "core self-test: FAILED (err %d)", (int)r);
    }
}

void app_main(void)
{
    logbuf_init(); /* first: every later log line goes to the PSRAM ring too */
    board_log_identity();

    ESP_ERROR_CHECK(init_nvs());
    fault_boot();
    if (fault_last_reset_abnormal()) {
        logbuf_print_previous();
    }
    ota_boot_report();
    ota_health_start();
    {
        const esp_err_t e = sv_start();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "supervisor: %s", esp_err_to_name(e));
        }
    }

    esp_err_t err = console_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    core_selftest();

    board_probe_t probe;
    err = board_probe_i2c(&probe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C probe: %s", esp_err_to_name(err));
    }

    /* Concentrator identification: read-only first, then one guarded reset. */
    conc_pin_survey();
    conc_id_t id = {0};
    err = conc_spi_init();
    if (err == ESP_OK) {
        err = conc_identify(&id);
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "no answer before reset; pulsing concentrator reset");
            err = conc_reset();
            if (err == ESP_OK) {
                err = conc_identify(&id);
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "concentrator probe: %s", esp_err_to_name(err));
    } else {
#if CONFIG_GW_CONC_SX1301
        const conc_chip_t built_for = CONC_CHIP_SX1301;
#else
        const conc_chip_t built_for = CONC_CHIP_SX1302;
#endif
        if (id.chip == built_for) {
            s_rx_queue = xQueueCreate(RADIO_QUEUE_LEN, sizeof(radio_pkt_t));
            g_radio_tx_req = xQueueCreate(RADIO_TX_QUEUE_LEN, sizeof(radio_tx_req_t));
            g_radio_tx_res = xQueueCreate(RADIO_TX_QUEUE_LEN, sizeof(radio_tx_res_t));
            err = (s_rx_queue && g_radio_tx_req && g_radio_tx_res) ? conc_hal_start(s_rx_queue)
                                                                    : ESP_ERR_NO_MEM;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "radio task: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "card chip (%d) does not match the build's HAL (%d): not starting it",
                     (int)id.chip, (int)built_for);
        }
    }

    /* May push a new image to the C6 and restart (bounded by an NVS counter). */
    err = c6_update_check();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 update check: %s", esp_err_to_name(err));
    }

    err = wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start: %s", esp_err_to_name(err));
    }

#if CONFIG_GW_OTA_TEST_HANG_BEFORE_START
    ESP_LOGW(TAG, "TEST BUILD: start-up stops here; the health check must roll back");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    /* Before anything that can fail late: this is the recovery path. */
    err = ota_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA server: %s", esp_err_to_name(err));
    }

    err = web_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web UI: %s", esp_err_to_name(err));
    }

    /* Opens BLE setup at once when there are no Wi-Fi credentials. */
    err = ble_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE setup: %s", esp_err_to_name(err));
    }

    err = led_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status LED: %s", esp_err_to_name(err));
    }

    /* Wall clock in every mode: packet timestamps, DeviceTimeAns, TLS. Started
     * before the network is up; SNTP retries, and asks again on a new IP. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_GW_NTP_SERVER);
    sntp.renew_servers_after_new_IP = true;
    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: no wall clock");
    }

    /* Gateway mode (ADR-0015): TTN mode forwards the radio queue; hybrid mode
     * forwards what the local server leaves; standalone forwards nothing. */
    QueueHandle_t fwd_q = NULL;
    err = lns_start(s_rx_queue, &fwd_q);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local network server: %s", esp_err_to_name(err));
    }
    /* Payload codecs (ADR-0021): before MQTT, which decodes with them. A
     * storage or codec failure leaves codecs off; the gateway runs on. */
    err = codecs_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codecs: %s", esp_err_to_name(err));
    }
    /* MQTT client (ADR-0018): events out, downlinks in. */
    err = mqttc_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT: %s", esp_err_to_name(err));
    }
    QueueHandle_t up_q = gwmode() == LWC_MODE_TTN ? s_rx_queue : fwd_q;
    /* Upstream link (ADR-0016): LoRa Basics Station. */
    if (up_q != NULL) {
        err = lbsfwd_start(up_q);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Basics Station client: %s", esp_err_to_name(err));
        }
    }

    err = eth_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start: %s", esp_err_to_name(err));
    }

    const sv_id_t sv = sv_register("main", SV_BUDGET_MS);
    for (;;) {
        sv_beat(sv);
        ESP_LOGI(TAG,
                 "heartbeat: wifi ip=%d, eth link=%d ip=%d, internal free %u (min %u), PSRAM free %u",
                 wifi_has_ip(), eth_link_up(), eth_has_ip(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}
