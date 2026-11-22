/*
 * SX1301 transmit path (lora_gateway HAL), run only from the radio task.
 *
 * - Power cap: the gain LUT is cut at CONFIG_GW_TX_MAX_DBM, so no request can
 *   exceed it (lgw_send picks the highest LUT entry <= the requested power).
 * - Timing: the counter is read live with GPS_EN=0 (no GPS on this gateway,
 *   as Semtech's forwarder does in timersync). Frames are admitted by the core
 *   JIT queue and handed to lgw_send TX_LEAD_US before their start.
 * - Safety: TX_FREE checked before each send; a frame still SCHEDULED after
 *   its end is aborted; the per-sub-band duty-cycle budget is enforced here.
 * - Card reset detection: after each frame, the counter must have advanced by
 *   the host-clock elapsed time; otherwise the caller restarts the HAL.
 */
#include "conc_tx.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "lwc/txpk.h"
#include "radio_tx.h"
#include "sdkconfig.h"

#ifndef CONFIG_GW_TX_TRACE
#define CONFIG_GW_TX_TRACE 0
#endif

static const char *TAG = "conc_tx";

#define TX_RF_CHAIN 0
#define TX_LEAD_US 40000U         /* hand to lgw_send this long before start */
#define TX_MIN_SEND_US 5000U      /* below this, too late to load the frame */
#define TX_MIN_ADMIT_US 20000U    /* reject on arrival below this */
#define TX_MAX_ADVANCE_US 10000000U /* TTN RX1 join delay is 5 s */
#define TX_GUARD_US TX_LEAD_US /* the next frame loads only once this one is FREE */
#define TX_END_MARGIN_US 100000 /* a sent frame still busy this long after its end is stuck */
#define TX_IMME_DELAY_US 60000U
#define TRACE_POLL_MS 1
#define COUNTER_TOLERANCE_US 50000
#define LORAWAN_PREAMBLE 8U /* the HAL's default; set explicitly so airtime matches */

/* Nebra global_conf.json.EU868 tx_lut_0..15 (dac_gain 3 as the HAL requires). */
static const struct lgw_tx_gain_s k_nebra_lut[] = {
    {.rf_power = -6, .pa_gain = 0, .mix_gain = 8, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = -3, .pa_gain = 0, .mix_gain = 10, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 0, .pa_gain = 0, .mix_gain = 12, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 3, .pa_gain = 1, .mix_gain = 8, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 6, .pa_gain = 1, .mix_gain = 10, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 10, .pa_gain = 1, .mix_gain = 12, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 11, .pa_gain = 1, .mix_gain = 13, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 12, .pa_gain = 2, .mix_gain = 9, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 13, .pa_gain = 1, .mix_gain = 15, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 14, .pa_gain = 2, .mix_gain = 10, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 16, .pa_gain = 2, .mix_gain = 11, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 20, .pa_gain = 3, .mix_gain = 9, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 23, .pa_gain = 3, .mix_gain = 10, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 25, .pa_gain = 3, .mix_gain = 11, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 26, .pa_gain = 3, .mix_gain = 12, .dig_gain = 0, .dac_gain = 3},
    {.rf_power = 27, .pa_gain = 3, .mix_gain = 14, .dig_gain = 0, .dac_gain = 3},
};

QueueHandle_t g_radio_tx_req;
QueueHandle_t g_radio_tx_res;
radio_tx_stats_t g_radio_tx_stats;

static int8_t s_min_dbm, s_max_dbm;
static bool s_counter_ok;
static lwc_jit_t s_jit;
static lwc_dutycycle_t s_dc;
static struct lgw_pkt_tx_s s_frames[LWC_JIT_SLOTS];
static bool s_trace[LWC_JIT_SLOTS];
static radio_tx_req_t s_req;

const char *radio_tx_result_str(radio_tx_result_t r)
{
    switch (r) {
    case RADIO_TX_OK: return "NONE";
    case RADIO_TX_TOO_LATE: return "TOO_LATE";
    case RADIO_TX_TOO_EARLY: return "TOO_EARLY";
    case RADIO_TX_COLLISION: return "COLLISION_PACKET";
    case RADIO_TX_FREQ: return "TX_FREQ";
    case RADIO_TX_GPS: return "GPS_UNLOCKED";
    case RADIO_TX_DUTY: return "DUTY_CYCLE";
    default: return "TOO_LATE";
    }
}

int conc_tx_configure(void)
{
#if CONFIG_GW_TX_ENABLE
    struct lgw_tx_gain_lut_s lut;
    memset(&lut, 0, sizeof lut);
    for (size_t i = 0; i < sizeof k_nebra_lut / sizeof k_nebra_lut[0]; i++) {
        if (k_nebra_lut[i].rf_power <= CONFIG_GW_TX_MAX_DBM) {
            lut.lut[lut.size++] = k_nebra_lut[i];
        }
    }
    if (lut.size == 0 || lgw_txgain_setconf(&lut) != LGW_HAL_SUCCESS) {
        ESP_LOGE(TAG, "TX gain LUT rejected");
        return -1;
    }
    s_min_dbm = lut.lut[0].rf_power;
    s_max_dbm = lut.lut[lut.size - 1].rf_power;
    ESP_LOGI(TAG, "TX on RF chain %d, %u gain steps %d..%d dBm (cap %d dBm)", TX_RF_CHAIN,
             lut.size, s_min_dbm, s_max_dbm, CONFIG_GW_TX_MAX_DBM);
#else
    ESP_LOGW(TAG, "TX disabled (CONFIG_GW_TX_ENABLE)");
#endif
    lwc_jit_init(&s_jit, TX_MIN_ADMIT_US, TX_MAX_ADVANCE_US, TX_GUARD_US);
    /* Once per boot: a HAL restart must not refill the hourly duty-cycle budget. */
    static bool dc_ready;
    if (!dc_ready) {
        lwc_dc_init(&s_dc, esp_timer_get_time());
        dc_ready = true;
    }
    return 0;
}

static bool counter_now(uint32_t *us)
{
    return lgw_get_trigcnt(us) == LGW_HAL_SUCCESS;
}

int conc_tx_after_start(void)
{
    s_counter_ok = false;
    /* GPS_EN=0: TIMESTAMP reads the live counter instead of the PPS latch. */
    if (lgw_reg_w(LGW_GPS_EN, 0) != LGW_REG_SUCCESS) {
        return -1;
    }
    uint32_t a, b;
    const int64_t h0 = esp_timer_get_time();
    if (!counter_now(&a)) {
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    const int64_t h1 = esp_timer_get_time();
    if (!counter_now(&b)) {
        return -1;
    }
    const int32_t dc = (int32_t)(b - a);
    const int64_t dh = h1 - h0;
    ESP_LOGI(TAG, "counter check: +%" PRId32 " us over %" PRId64 " us host time", dc, dh);
    if (dc < dh - COUNTER_TOLERANCE_US || dc > dh + COUNTER_TOLERANCE_US) {
        ESP_LOGE(TAG, "concentrator counter is not free-running: TX disabled");
        return -1;
    }
    s_counter_ok = true;
    return 0;
}

static uint8_t bw_code(uint16_t khz)
{
    return khz == 500U ? BW_500KHZ : khz == 250U ? BW_250KHZ : BW_125KHZ;
}

static uint8_t cr_code(uint8_t d)
{
    return d == 8U ? CR_LORA_4_8 : d == 7U ? CR_LORA_4_7 : d == 6U ? CR_LORA_4_6 : CR_LORA_4_5;
}

static void reply(const radio_tx_req_t *r, radio_tx_result_t res, int8_t pw, bool capped)
{
    if (r->test) {
        ESP_LOGI(TAG, "test frame: %s", radio_tx_result_str(res));
        return;
    }
    const radio_tx_res_t out = {r->token, res, pw, capped};
    (void)xQueueSend(g_radio_tx_res, &out, 0);
}

static void admit(const radio_tx_req_t *r)
{
#if !CONFIG_GW_TX_ENABLE
    reply(r, RADIO_TX_FREQ, 0, false);
    return;
#else
    const lwc_txpk_t *p = &r->pk;
    if (!s_counter_ok) {
        reply(r, RADIO_TX_BUSY, 0, false);
        return;
    }
    if (p->mode == LWC_TXPK_GPS) {
        reply(r, RADIO_TX_GPS, 0, false);
        return;
    }
    if (lwc_dc_band(p->freq_hz) < 0) { /* inside an EU868 sub-band, so inside 863-870 MHz */
        reply(r, RADIO_TX_FREQ, 0, false);
        return;
    }
    int8_t pw = p->powe;
    bool capped = false;
    if (pw > s_max_dbm) {
        pw = s_max_dbm;
        capped = true;
    }
    if (pw < s_min_dbm) {
        pw = s_min_dbm;
    }

    uint32_t now;
    if (!counter_now(&now)) {
        reply(r, RADIO_TX_BUSY, 0, false);
        return;
    }
    struct lgw_pkt_tx_s f;
    memset(&f, 0, sizeof f);
    f.freq_hz = p->freq_hz;
    f.tx_mode = TIMESTAMPED;
    f.count_us = (p->mode == LWC_TXPK_IMMEDIATE) ? now + TX_IMME_DELAY_US : p->tmst;
    f.rf_chain = TX_RF_CHAIN;
    f.rf_power = pw;
    f.modulation = MOD_LORA;
    f.bandwidth = bw_code(p->bw_khz);
    f.datarate = (uint32_t)(1U << (p->sf - 6U));
    f.coderate = cr_code(p->cr_denom);
    f.invert_pol = p->ipol;
    f.preamble = p->prea ? p->prea : LORAWAN_PREAMBLE;
    f.no_crc = p->ncrc;
    f.no_header = false;
    f.size = p->size;
    memcpy(f.payload, p->payload, p->size);
    /* Core airtime, not lgw_time_on_air: the HAL's version uses the request's
     * preamble field, which is 0 for "default" while lgw_send sends 8. */
    const uint32_t air_us = lwc_lora_airtime_us(p->sf, p->bw_khz, p->cr_denom, f.preamble,
                                                f.size, !f.no_crc, false);

    size_t slot = 0;
    const lwc_jit_res_t j = lwc_jit_admit(&s_jit, now, f.count_us, air_us, r->token, &slot);
    if (j != LWC_JIT_OK) {
        const radio_tx_result_t res = j == LWC_JIT_TOO_LATE ? RADIO_TX_TOO_LATE
                                      : j == LWC_JIT_TOO_EARLY ? RADIO_TX_TOO_EARLY
                                                               : RADIO_TX_COLLISION;
        ESP_LOGW(TAG, "downlink refused: %s (start in %" PRId32 " us)", radio_tx_result_str(res),
                 (int32_t)(f.count_us - now));
        reply(r, res, pw, capped);
        return;
    }
    if (!lwc_dc_take(&s_dc, esp_timer_get_time(), f.freq_hz, air_us)) {
        lwc_jit_release(&s_jit, slot);
        ESP_LOGW(TAG, "downlink refused: duty-cycle budget used up on %" PRIu32 " Hz", f.freq_hz);
        reply(r, RADIO_TX_DUTY, pw, capped);
        return;
    }
    s_frames[slot] = f;
    s_trace[slot] = r->test || CONFIG_GW_TX_TRACE;
    ESP_LOGI(TAG, "downlink queued: %" PRIu32 " Hz SF%u %d dBm%s, %u B, %" PRIu32
                  " us airtime, start in %" PRId32 " us",
             f.freq_hz, p->sf, pw, capped ? " (capped)" : "", f.size, air_us,
             (int32_t)(f.count_us - now));
    reply(r, RADIO_TX_OK, pw, capped);
#endif
}

/* Follow one frame through SCHEDULED -> EMITTING -> FREE; abort if stuck.
 * Returns false if the counter afterwards does not match the host clock. */
static bool trace_frame(const struct lgw_pkt_tx_s *f, uint32_t air_us)
{
    uint8_t st = TX_STATUS_UNKNOWN, last = 0xFF;
    uint32_t c0;
    const int64_t h0 = esp_timer_get_time();
    if (!counter_now(&c0)) {
        return false;
    }
    const int64_t deadline_us = (int64_t)(int32_t)(f->count_us - c0) + air_us + 200000;
    for (;;) {
        uint32_t c;
        if (lgw_status(TX_STATUS, &st) != LGW_HAL_SUCCESS || !counter_now(&c)) {
            return false;
        }
        if (st != last) {
            ESP_LOGI(TAG, "  tx status %s at counter %+" PRId32 " us from start",
                     st == TX_FREE ? "FREE" : st == TX_SCHEDULED ? "SCHEDULED"
                     : st == TX_EMITTING ? "EMITTING" : "OTHER",
                     (int32_t)(c - f->count_us));
            last = st;
        }
        if (st == TX_FREE && (int32_t)(c - f->count_us) > 0) {
            break;
        }
        if (esp_timer_get_time() - h0 > deadline_us) {
            ESP_LOGE(TAG, "TX still %s after its end: aborting",
                     st == TX_SCHEDULED ? "SCHEDULED" : "busy");
            (void)lgw_abort_tx();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TRACE_POLL_MS));
    }
    /* Card reset check: the counter must have advanced with the host clock. */
    uint32_t c1;
    if (!counter_now(&c1)) {
        return false;
    }
    const int64_t dh = esp_timer_get_time() - h0;
    const int32_t dc = (int32_t)(c1 - c0);
    if (dc < dh - COUNTER_TOLERANCE_US || dc > dh + COUNTER_TOLERANCE_US) {
        ESP_LOGE(TAG, "counter jumped (+%" PRId32 " us vs %" PRId64 " us host): card reset?",
                 dc, dh);
        return false;
    }
    return true;
}

bool conc_tx_service(void)
{
    while (g_radio_tx_req != NULL && xQueueReceive(g_radio_tx_req, &s_req, 0) == pdTRUE) {
        admit(&s_req);
    }
    if (!s_counter_ok) {
        return true;
    }

    uint32_t now;
    if (!counter_now(&now)) {
        return false;
    }
    /* A sent frame blocks new ones until it ends; then check the radio let go. */
    const int ended = lwc_jit_ended(&s_jit, now, TX_END_MARGIN_US);
    if (ended >= 0) {
        uint8_t est = TX_STATUS_UNKNOWN;
        if (lgw_status(TX_STATUS, &est) != LGW_HAL_SUCCESS) {
            return false;
        }
        if (est != TX_FREE) {
            ESP_LOGE(TAG, "TX still busy %d us after its end: aborting", TX_END_MARGIN_US);
            (void)lgw_abort_tx();
        }
        lwc_jit_release(&s_jit, (size_t)ended);
    }
    int expired = -1;
    const int due = lwc_jit_due(&s_jit, now, TX_LEAD_US, TX_MIN_SEND_US, &expired);
    if (expired >= 0) {
        ESP_LOGW(TAG, "downlink dropped: missed its start (%" PRId32 " us late)",
                 (int32_t)(now - s_jit.slot[expired].start));
        atomic_fetch_add(&g_radio_tx_stats.txdrop, 1U);
        lwc_jit_release(&s_jit, (size_t)expired);
    }
    if (due < 0) {
        return true;
    }
    uint8_t st = TX_STATUS_UNKNOWN;
    if (lgw_status(TX_STATUS, &st) != LGW_HAL_SUCCESS) {
        return false;
    }
    if (st != TX_FREE) {
        return true; /* try again next loop; the expiry check drops it if needed */
    }
    const struct lgw_pkt_tx_s *f = &s_frames[due];
    const uint32_t air_us = s_jit.slot[due].airtime_us;
    const int64_t t0 = esp_timer_get_time();
    const int rc = lgw_send(*f);
    const int64_t send_us = esp_timer_get_time() - t0;
    const bool trace = s_trace[due];
    const int32_t lead = (int32_t)(f->count_us - now);
    if (rc != LGW_HAL_SUCCESS) {
        lwc_jit_release(&s_jit, (size_t)due);
        ESP_LOGE(TAG, "lgw_send failed");
        atomic_fetch_add(&g_radio_tx_stats.txdrop, 1U);
        return true;
    }
    lwc_jit_sent(&s_jit, (size_t)due);
    atomic_fetch_add(&g_radio_tx_stats.txnb, 1U);
    ESP_LOGI(TAG, "TX handed over %" PRId32 " us before start; lgw_send took %" PRId64 " us",
             lead, send_us);
    return trace ? trace_frame(f, air_us) : true;
}
