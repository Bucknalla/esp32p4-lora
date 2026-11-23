/*
 * Concentrator task (core 1, the radio core).
 *
 * SX1301 (lora_gateway): Nebra's EU868 config, lgw_start (firmware load +
 * radio calibration), then a receive loop that queues each packet
 * (radio_pkt_t) for the local server or the Basics Station client. Downlinks: conc_tx.c, same task.
 * Recovery: a failed lgw_start or lgw_receive stops the HAL, resets the
 * concentrator and starts again after a backoff.
 * SX1302 (sx1302_hal): one-shot util_chip_id check (lgw_start, EUI,
 * temperature, lgw_stop); not yet wired to the forwarder.
 */
#include "conc_hal.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

#include "conc.h"
#include "conc_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loragw_hal.h"
#include "radio_pkt.h"
#include "sv.h"
#include "sdkconfig.h"

#ifndef CONFIG_GW_TX_ENABLE
#define CONFIG_GW_TX_ENABLE 0
#endif

static const char *TAG = "conc_hal";

/* load_firmware keeps an 8 KB verify buffer on the stack; measured use with
 * lgw_start was ~9 KB of 24 KB. */
#define HAL_TASK_STACK 16384
#define HAL_TASK_PRIO 10
#define HAL_TASK_CORE 1

#define EU868_RADIO0_HZ CONC_EU868_RADIO0_HZ
#define EU868_RADIO1_HZ CONC_EU868_RADIO1_HZ

radio_stats_t g_radio_stats;

#if CONFIG_GW_CONC_SX1301

#define SX1257_RSSI_OFFSET (-158.0f) /* Nebra global_conf.json.EU868 */
#define RX_BATCH 8
#define RX_POLL_MS 10
#define BACKOFF_MIN_MS 2000U
#define BACKOFF_MAX_MS 60000U
#define WALLCLOCK_VALID_S 1700000000LL /* after 2023-11: SNTP has set the clock */

/* EU868 channel plan (Nebra global_conf.json.EU868): IF chains 0-7 multi-SF
 * 125 kHz, chain 8 LoRa std SF7 250 kHz, chain 9 FSK off. */
static const struct {
    uint8_t rf_chain;
    int32_t if_hz;
} k_multisf[8] = {
    {1, -400000}, {1, -200000}, {1, 0},      {0, -400000},
    {0, -200000}, {0, 0},       {0, 200000}, {0, 400000},
};

static struct lgw_pkt_rx_s s_rx[RX_BATCH]; /* ~2.3 KB: static, not on the stack */
static radio_pkt_t s_item;                 /* queue staging, copied by xQueueSend */
static QueueHandle_t s_queue;

static int configure_sx1301(void)
{
    struct lgw_conf_board_s board;
    memset(&board, 0, sizeof board);
    board.lorawan_public = true;
    board.clksrc = CONFIG_GW_CONC_CLKSRC;
    if (lgw_board_setconf(board) != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "lgw_board_setconf failed");
        return -1;
    }

    const uint32_t rf_hz[2] = {EU868_RADIO0_HZ, EU868_RADIO1_HZ};
    for (uint8_t chain = 0; chain < 2; chain++) {
        struct lgw_conf_rxrf_s rf;
        memset(&rf, 0, sizeof rf);
        rf.enable = true;
        rf.freq_hz = rf_hz[chain];
        rf.rssi_offset = SX1257_RSSI_OFFSET;
        rf.type = LGW_RADIO_TYPE_SX1257;
        rf.tx_enable = (chain == 0) && CONFIG_GW_TX_ENABLE; /* Nebra: TX on radio_0 */
        if (lgw_rxrf_setconf(chain, rf) != LGW_HAL_SUCCESS) {
            ESP_LOGE(TAG, "lgw_rxrf_setconf(%u) failed", chain);
            return -1;
        }
    }

    for (uint8_t i = 0; i < 8; i++) {
        struct lgw_conf_rxif_s ifc;
        memset(&ifc, 0, sizeof ifc);
        ifc.enable = true;
        ifc.rf_chain = k_multisf[i].rf_chain;
        ifc.freq_hz = k_multisf[i].if_hz;
        ifc.bandwidth = BW_125KHZ;
        ifc.datarate = DR_LORA_MULTI;
        if (lgw_rxif_setconf(i, ifc) != LGW_HAL_SUCCESS) {
            ESP_LOGE(TAG, "lgw_rxif_setconf(%u) failed", i);
            return -1;
        }
    }

    struct lgw_conf_rxif_s std;
    memset(&std, 0, sizeof std);
    std.enable = true;
    std.rf_chain = 1;
    std.freq_hz = -200000;
    std.bandwidth = BW_250KHZ;
    std.datarate = DR_LORA_SF7;
    if (lgw_rxif_setconf(8, std) != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "lgw_rxif_setconf(8) failed");
        return -1;
    }
    return 0;
}


static uint8_t sf_of(uint32_t dr)
{
    for (uint8_t sf = 7; sf <= 12; sf++) {
        if (dr == (uint32_t)(1U << (sf - 6U))) { /* DR_LORA_SF7 = 0x02 ... SF12 = 0x40 */
            return sf;
        }
    }
    return 0;
}

static uint16_t bw_khz_of(uint8_t bw)
{
    switch (bw) {
    case BW_125KHZ: return 125;
    case BW_250KHZ: return 250;
    case BW_500KHZ: return 500;
    default: return 0;
    }
}

static uint8_t cr_of(uint8_t cr)
{
    switch (cr) {
    case CR_LORA_4_5: return 5;
    case CR_LORA_4_6: return 6;
    case CR_LORA_4_7: return 7;
    case CR_LORA_4_8: return 8;
    default: return 0;
    }
}

static int64_t wallclock_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < WALLCLOCK_VALID_S) {
        return -1;
    }
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* Convert and queue one packet. Never blocks: a full queue counts a drop. */
static void handle_packet(const struct lgw_pkt_rx_s *p, int64_t now_us)
{
    atomic_fetch_add(&g_radio_stats.rxnb, 1U);
    if (p->status == STAT_CRC_OK) {
        atomic_fetch_add(&g_radio_stats.rxok, 1U);
    }
    const uint8_t sf = sf_of(p->datarate);
    const uint16_t bw = bw_khz_of(p->bandwidth);
    const uint8_t cr = cr_of(p->coderate);
    ESP_LOGI(TAG, "rx %" PRIu32 " Hz SF%u BW%u %s RSSI %.0f SNR %.1f %u B", p->freq_hz, sf, bw,
             p->status == STAT_CRC_OK ? "CRC ok" : p->status == STAT_CRC_BAD ? "CRC bad" : "no CRC",
             (double)p->rssi, (double)p->snr, p->size);
    if (p->modulation != MOD_LORA || sf == 0U || bw == 0U || cr == 0U || p->size > 255U) {
        return;
    }
    s_item.time_unix_us = now_us;
    s_item.tmst = p->count_us;
    s_item.freq_hz = p->freq_hz;
    s_item.chan = p->if_chain;
    s_item.rfch = p->rf_chain;
    s_item.stat = (p->status == STAT_CRC_OK) ? 1 : (p->status == STAT_CRC_BAD) ? -1 : 0;
    s_item.sf = sf;
    s_item.bw_khz = bw;
    s_item.cr_denom = cr;
    s_item.rssi = (int16_t)(p->rssi < 0 ? p->rssi - 0.5f : p->rssi + 0.5f);
    s_item.lsnr = p->snr;
    s_item.size = p->size;
    memcpy(s_item.payload, p->payload, p->size);
    if (xQueueSend(s_queue, &s_item, 0) != pdTRUE) {
        atomic_fetch_add(&g_radio_stats.dropped, 1U);
    }
}

/* Watchdog budget: lgw_start takes ~3.2 s; everything else beats. */
#define SV_BUDGET_MS 30000
static sv_id_t s_sv = -1;

/* Returns when the HAL fails; the caller recovers. */
static void receive_loop(void)
{
    for (;;) {
        sv_beat(s_sv);
        if (!conc_tx_service()) {
            ESP_LOGE(TAG, "TX path lost the concentrator: restarting it");
            return;
        }
        const int n = lgw_receive(RX_BATCH, s_rx);
        if (n < 0) {
            ESP_LOGE(TAG, "lgw_receive failed");
            return;
        }
        if (n > 0) {
            const int64_t now = wallclock_us();
            for (int i = 0; i < n; i++) {
                handle_packet(&s_rx[i], now);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    uint32_t backoff = BACKOFF_MIN_MS;
    s_sv = sv_register("radio", SV_BUDGET_MS);
    for (;;) {
        sv_beat(s_sv);
        if (conc_reset() == ESP_OK && configure_sx1301() == 0 && conc_tx_configure() == 0) {
            const TickType_t t0 = xTaskGetTickCount();
            if (lgw_start() == LGW_HAL_SUCCESS) {
                (void)conc_tx_after_start();
                atomic_fetch_add(&g_radio_stats.starts, 1U);
                ESP_LOGI(TAG, "lgw_start OK in %" PRIu32 " ms; receiving on EU868",
                         (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
                ESP_LOGI(TAG, "stack high-water mark: %u bytes free of %u",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)HAL_TASK_STACK);
                backoff = BACKOFF_MIN_MS;
                receive_loop();
                (void)lgw_stop();
            } else {
                ESP_LOGE(TAG, "lgw_start failed; see DEBUG_HAL lines above");
            }
        }
        atomic_fetch_add(&g_radio_stats.restarts, 1U);
        ESP_LOGW(TAG, "concentrator restart in %u ms", (unsigned)backoff);
        sv_delay(s_sv, backoff);
        backoff = (backoff * 2U > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : backoff * 2U;
    }
}

esp_err_t conc_hal_start(QueueHandle_t rx_queue)
{
    if (rx_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_queue = rx_queue;
    BaseType_t ok = xTaskCreatePinnedToCore(radio_task, "radio", HAL_TASK_STACK, NULL,
                                            HAL_TASK_PRIO, NULL, HAL_TASK_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

#elif CONFIG_GW_CONC_SX1302

static void run_check(void)
{
    struct lgw_conf_board_s board;
    memset(&board, 0, sizeof board);
    board.lorawan_public = true;
    board.clksrc = CONFIG_GW_CONC_CLKSRC;
    board.full_duplex = false;
    board.com_type = LGW_COM_SPI;
    strncpy(board.com_path, "/dev/spidev0.0", sizeof board.com_path - 1);
    if (lgw_board_setconf(&board) != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "lgw_board_setconf failed");
        return;
    }
    const uint32_t rf_hz[2] = {EU868_RADIO0_HZ, EU868_RADIO1_HZ};
    for (uint8_t chain = 0; chain < 2; chain++) {
        struct lgw_conf_rxrf_s rf;
        memset(&rf, 0, sizeof rf);
        rf.enable = (chain == board.clksrc); /* the clock-source radio must run */
        rf.freq_hz = rf_hz[chain];
        rf.type = LGW_RADIO_TYPE_SX1250;
        rf.tx_enable = false;
        if (lgw_rxrf_setconf(chain, &rf) != LGW_HAL_SUCCESS) {
            ESP_LOGE(TAG, "lgw_rxrf_setconf(%u) failed", chain);
            return;
        }
    }
    const TickType_t t0 = xTaskGetTickCount();
    if (lgw_start() != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "lgw_start failed");
        return;
    }
    ESP_LOGI(TAG, "lgw_start OK in %" PRIu32 " ms",
             (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
    uint64_t eui = 0;
    if (lgw_get_eui(&eui) == LGW_HAL_SUCCESS) {
        ESP_LOGI(TAG, "concentrator EUI: %016" PRIx64, eui);
    }
    float temp = 0.0f;
    if (lgw_get_temperature(&temp) == LGW_HAL_SUCCESS) {
        ESP_LOGI(TAG, "temperature: %.1f C", (double)temp);
    }
    if (lgw_stop() != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "lgw_stop failed");
    }
}


static void hal_check_task(void *arg)
{
    (void)arg;
    if (conc_reset() == ESP_OK) {
        run_check();
    } else {
        ESP_LOGE(TAG, "reset failed");
    }
    ESP_LOGI(TAG, "stack high-water mark: %u bytes free of %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)HAL_TASK_STACK);
    vTaskDelete(NULL);
}

/* SX1302: one-shot check only; packets are not forwarded yet. */
esp_err_t conc_hal_start(QueueHandle_t rx_queue)
{
    (void)rx_queue;
    BaseType_t ok = xTaskCreatePinnedToCore(hal_check_task, "conc_hal", 24576, NULL,
                                            HAL_TASK_PRIO, NULL, HAL_TASK_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif
