/*
 * Local network server task (ADR-0015). Reads the radio queue in standalone
 * and hybrid modes, runs joins and uplinks through the core server
 * (core/src/ns.c), stores devices and counters in NVS, and in hybrid mode
 * hands every frame it does not handle to the Basics Station client.
 *
 * Transmit: join-accepts and class A answers (ACK, MAC commands) go through
 * the radio task's JIT queue, duty-cycle budget and power cap, tagged
 * RADIO_SRC_LNS, in RX1 with RX2 as the fallback. Until local transmit is
 * switched on (Config tab, ADR-0017; default CONFIG_GW_LNS_TX), they are only
 * logged: dry run.
 *
 * Keys: AppKeys and session keys are in plain NVS (ADR-0002); they are never
 * logged or returned by the API.
 */
#include "lns.h"
#include "radioplan.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gwmode.h"
#include "lwc/hex.h"
#include "lwc/lorawan.h"
#include "lwc/mac.h"
#include "nvs.h"
#include "radio_pkt.h"
#include "radio_tx.h"
#include "sdkconfig.h"
#include "sv.h"

static bool lns_tx(void)
{
    radio_tx_cfg_t c;
    radioplan_tx(&c);
    return c.lns_tx;
}

static const char *TAG = "lns";

#define TASK_STACK 6144
#define TASK_PRIO 4
#define TASK_CORE 1
#define NVS_NS "lns"
#define META_VER 1U /* "meta" record: JoinNonce and DevAddr allocator (never reset) */
#define DEV_VER 2U  /* device record: 2 adds the receive windows (ADR-0017 step 5); 1 is migrated */
#define FCNT_DOWN_SKIP 16U /* downlink counter skip at boot: never reuse one */
#define DOWN_POWER_DBM 14  /* EU868 RX1 default; the radio caps it (ADR-0007) */
#define PAYLOAD_SHOW 32U

typedef struct {
    uint8_t ver;
    uint32_t join_nonce;
    uint32_t next_addr;
} meta_rec_t;

typedef struct {
    uint8_t ver;
    lwc_ns_dev_t d;
} dev_rec_t;

/* Device record version 1, frozen: lwc_ns_dev_t before the receive windows.
 * Read once at boot and written back as version 2. */
typedef struct {
    bool used;
    char name[LWC_NS_NAME_MAX + 1U];
    uint64_t dev_eui;
    uint64_t join_eui;
    uint8_t app_key[16];
    uint8_t mac;
    uint16_t nonces[LWC_NS_NONCE_HISTORY];
    uint8_t nonce_count;
    uint8_t nonce_pos;
    bool joined;
    uint32_t dev_addr;
    uint8_t nwk_s_key[16];
    uint8_t app_s_key[16];
    bool has_up;
    uint32_t fcnt_up;
    uint32_t fcnt_down;
} dev_v1_t;

typedef struct {
    uint8_t ver;
    dev_v1_t d;
} dev_rec_v1_t;

/* Version 2 only appends fields: every version 1 field keeps its offset. */
_Static_assert(offsetof(dev_rec_t, d) == offsetof(dev_rec_v1_t, d), "record header");
_Static_assert(offsetof(lwc_ns_dev_t, nonces) == offsetof(dev_v1_t, nonces), "v1 layout");
_Static_assert(offsetof(lwc_ns_dev_t, app_s_key) == offsetof(dev_v1_t, app_s_key), "v1 layout");
_Static_assert(offsetof(lwc_ns_dev_t, fcnt_down) == offsetof(dev_v1_t, fcnt_down), "v1 layout");

static void from_v1(const dev_v1_t *o, lwc_ns_dev_t *d)
{
    memset(d, 0, sizeof *d);
    d->used = o->used;
    memcpy(d->name, o->name, sizeof d->name);
    d->dev_eui = o->dev_eui;
    d->join_eui = o->join_eui;
    memcpy(d->app_key, o->app_key, sizeof d->app_key);
    d->mac = o->mac;
    memcpy(d->nonces, o->nonces, sizeof d->nonces);
    d->nonce_count = o->nonce_count;
    d->nonce_pos = o->nonce_pos;
    d->joined = o->joined;
    d->dev_addr = o->dev_addr;
    memcpy(d->nwk_s_key, o->nwk_s_key, sizeof d->nwk_s_key);
    memcpy(d->app_s_key, o->app_s_key, sizeof d->app_s_key);
    d->has_up = o->has_up;
    d->fcnt_up = o->fcnt_up;
    d->fcnt_down = o->fcnt_down;
    /* version 1 devices joined with the defaults */
    d->rx1_delay_s = 1;
    d->rx2_dr = 0;
    d->rx2_hz = LWC_NS_RX2_FREQ_HZ;
}

typedef struct {
    uint32_t fcnt_up;
    uint32_t fcnt_down;
    uint8_t has_up;
} cnt_rec_t;

/* What the UI shows per device; RAM only, lost at restart. */
typedef struct {
    int64_t seen_us; /* 0: never since boot */
    uint32_t uplinks;
    int16_t rssi;
    float snr;
    uint8_t sf;
    bool has_port;
    uint8_t port;
    uint8_t len;
    uint8_t payload[PAYLOAD_SHOW];
    bool join_unanswered; /* dry run: a join-request was accepted but not answered */
} dev_stat_t;

static lwc_ns_t s_ns;
static dev_stat_t s_stat[LWC_NS_MAX_DEVICES];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_radio_q;
static QueueHandle_t s_fwd_q;
static atomic_bool s_running;
static atomic_uint s_joins, s_uplinks, s_downs, s_fwd, s_fwd_drop;
static uint16_t s_tx_token;

bool lns_running(void)
{
    return atomic_load(&s_running);
}

/* ---- storage ------------------------------------------------------------------ */

static void key_of(char out[4], char kind, size_t i)
{
    (void)snprintf(out, 4, "%c%02u", kind, (unsigned)i);
}

static esp_err_t save_blob(const char *key, const void *v, size_t len)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_blob(h, key, v, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void erase_key(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "erase %s: %s", key, esp_err_to_name(err));
    }
}

/* Unused in a dry-run build, which stores no join. */
static esp_err_t save_meta(void)
{
    const meta_rec_t m = {META_VER, s_ns.join_nonce, s_ns.next_addr};
    return save_blob("meta", &m, sizeof m);
}

static esp_err_t save_dev(size_t i)
{
    char k[4];
    key_of(k, 'd', i);
    dev_rec_t *r = calloc(1, sizeof *r);
    ESP_RETURN_ON_FALSE(r != NULL, ESP_ERR_NO_MEM, TAG, "rec");
    r->ver = DEV_VER;
    r->d = s_ns.dev[i];
    const esp_err_t err = save_blob(k, r, sizeof *r);
    memset(r, 0, sizeof *r); /* keys */
    free(r);
    return err;
}

static esp_err_t save_cnt(size_t i)
{
    char k[4];
    key_of(k, 'c', i);
    const cnt_rec_t c = {s_ns.dev[i].fcnt_up, s_ns.dev[i].fcnt_down, s_ns.dev[i].has_up ? 1U : 0U};
    return save_blob(k, &c, sizeof c);
}

static void load(void)
{
    lwc_ns_init(&s_ns);
    lwc_plan_t plan;
    radioplan_get(&plan);
    lwc_ns_set_plan(&s_ns, &plan);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no devices yet");
        return;
    }
    meta_rec_t m;
    size_t len = sizeof m;
    if (nvs_get_blob(h, "meta", &m, &len) == ESP_OK && len == sizeof m && m.ver == META_VER) {
        s_ns.join_nonce = m.join_nonce;
        s_ns.next_addr = m.next_addr;
    }
    dev_rec_t *r = calloc(1, sizeof *r);
    unsigned n = 0;
    bool migrate[LWC_NS_MAX_DEVICES] = {false};
    for (size_t i = 0; r != NULL && i < LWC_NS_MAX_DEVICES; i++) {
        char k[4];
        key_of(k, 'd', i);
        len = sizeof *r;
        if (nvs_get_blob(h, k, r, &len) != ESP_OK) {
            continue;
        }
        if (len == sizeof *r && r->ver == DEV_VER && r->d.used) {
            s_ns.dev[i] = r->d;
        } else if (len == sizeof(dev_rec_v1_t) && r->ver == 1U && ((const dev_rec_v1_t *)r)->d.used) {
            from_v1(&((const dev_rec_v1_t *)r)->d, &s_ns.dev[i]);
            migrate[i] = true;
        } else {
            ESP_LOGW(TAG, "device record %s has another format: skipped", k);
            continue;
        }
        cnt_rec_t c;
        key_of(k, 'c', i);
        len = sizeof c;
        if (nvs_get_blob(h, k, &c, &len) == ESP_OK && len == sizeof c) {
            s_ns.dev[i].fcnt_up = c.fcnt_up;
            s_ns.dev[i].fcnt_down = c.fcnt_down;
            s_ns.dev[i].has_up = c.has_up != 0U;
        }
        s_ns.dev[i].fcnt_down += FCNT_DOWN_SKIP;
        n++;
    }
    if (r != NULL) {
        memset(r, 0, sizeof *r);
        free(r);
    }
    nvs_close(h);
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        if (s_ns.dev[i].used) {
            (void)save_cnt(i); /* the skipped downlink counter */
        }
        if (migrate[i]) {
            const esp_err_t err = save_dev(i);
            ESP_LOGW(TAG, "device %s: record version 1 -> 2%s", s_ns.dev[i].name,
                     err == ESP_OK ? "" : " (not written: read again at the next boot)");
        }
    }
    ESP_LOGI(TAG, "%u device(s) loaded", n);
}

/* ---- frames ------------------------------------------------------------------- */

static const char *eui_str(uint64_t e, char out[17])
{
    uint8_t b[8];
    for (size_t i = 0; i < 8U; i++) {
        b[i] = (uint8_t)(e >> (56U - 8U * i));
    }
    lwc_hex_encode(b, 8, out);
    return out;
}

/* Milliseconds from the uplink's arrival to now, or -1 without a wall clock:
 * RX1 opens 1 s (join: 5 s) after the uplink. */
static int32_t since_uplink_ms(const radio_pkt_t *p)
{
    struct timeval tv;
    if (p->time_unix_us < 0 || gettimeofday(&tv, NULL) != 0) {
        return -1;
    }
    const int64_t now = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    return (int32_t)((now - p->time_unix_us) / 1000LL);
}

/* Queue a class A downlink: RX1 `delay_us` after the uplink, RX2 one second
 * later if RX1 is refused. `what` names it in the log. */
static void send_down(const radio_pkt_t *p, const uint8_t *frame, size_t len, uint32_t delay_us, uint32_t rx2_hz,
                      uint8_t rx2_dr, const char *what, uint64_t dev_eui)
{
    static radio_tx_req_t req;
    memset(&req, 0, sizeof req);
    req.source = RADIO_SRC_LNS;
    req.token = ++s_tx_token;
    lwc_ns_downlink(false, p->tmst, p->freq_hz, p->sf, p->bw_khz, delay_us, rx2_hz, rx2_dr, DOWN_POWER_DBM, frame, len,
                    &req.pk);
    lwc_txpk_t rx2;
    lwc_ns_downlink(true, p->tmst, p->freq_hz, p->sf, p->bw_khz, delay_us, rx2_hz, rx2_dr, DOWN_POWER_DBM, NULL, 0,
                    &rx2);
    req.alt = (radio_tx_alt_t){.on = true, .tmst = rx2.tmst, .freq_hz = rx2.freq_hz, .sf = rx2.sf,
                               .bw_khz = rx2.bw_khz};
    char e[17];
    const int32_t late = since_uplink_ms(p);
    if (!lns_tx()) {
        ESP_LOGW(TAG, "DRY RUN: %s for %s (%u B) at RX1 %" PRIu32 " Hz SF%u tmst %" PRIu32 ", %" PRId32
                      " ms after the uplink, not sent (local transmit off)",
                 what, eui_str(dev_eui, e), (unsigned)len, req.pk.freq_hz, req.pk.sf, req.pk.tmst, late);
        return;
    }
    if (g_radio_tx_req == NULL || xQueueSend(g_radio_tx_req, &req, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "%s for %s: radio queue full", what, eui_str(dev_eui, e));
        return;
    }
    atomic_fetch_add(&s_downs, 1U);
    ESP_LOGI(TAG, "%s %u for %s: RX1 %" PRIu32 " Hz SF%u, queued %" PRId32 " ms after the uplink", what,
             (unsigned)req.token, eui_str(dev_eui, e), req.pk.freq_hz, req.pk.sf, late);
}

/* After an accepted or repeated uplink: build the class A answer if the
 * device needs one (this advances the downlink counter). Called with s_lock
 * held; the caller stores the counters before sending. */
static bool answer(const radio_pkt_t *p, lwc_ns_up_res_t r, const lwc_ns_uplink_t *up, lwc_ns_dl_t *dl)
{
    lwc_ns_rx_t rx = {
        .snr10 = (int16_t)lroundf(p->lsnr * 10.0f),
        .sf = p->sf,
        .bw_khz = p->bw_khz,
        .now_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
    };
    rx.time_ok = lwc_gps_from_unix_us(p->time_unix_us, &rx.gps_s, &rx.gps_frac);
    return lwc_ns_answer(&s_ns, r, up, &rx, dl);
}

static void describe(const lwc_ns_dl_t *dl, char *out, size_t cap)
{
    char adr[40] = "";
    if (dl->link_adr) {
        (void)snprintf(adr, sizeof adr, " LinkADRReq(DR%u, power -%u dB)", dl->adr_dr, 2U * dl->adr_power);
    }
    (void)snprintf(out, cap, "downlink FCnt %" PRIu32 "%s%s%s%s%s", dl->fcnt, dl->ack ? " ACK" : "",
                   dl->link_check ? " LinkCheckAns" : "", dl->device_time ? " DeviceTimeAns" : "", adr,
                   dl->dev_status ? " DevStatusReq" : "");
}

/* Returns true when the frame was handled here (not to be forwarded). */
static bool handle(const radio_pkt_t *p)
{
    if (p->stat != 1) {
        return false;
    }
    static uint8_t acc[40];
    static lwc_ns_uplink_t up;
    static lwc_ns_dl_t dl;
    char e[17];
    const int mt = lwc_lw_mtype(p->payload, p->size);
    bool handled = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mt == LWC_LW_JOIN_REQUEST) {
        size_t alen = 0, idx = 0;
        static lwc_plan_t plan;
        radioplan_get(&plan); /* the CFList follows the radio's plan (ADR-0017) */
        lwc_ns_set_plan(&s_ns, &plan);
        radio_rx_cfg_t rxw;
        radioplan_rx(&rxw);
        (void)lwc_ns_set_rx(&s_ns, rxw.rx1_delay_s, rxw.rx2_hz, rxw.rx2_dr);
        const lwc_ns_join_res_t r = lwc_ns_join(&s_ns, p->payload, p->size, acc, sizeof acc, &alen, &idx);
        if (r == LWC_NS_JOIN_ACCEPT) {
            const uint64_t eui = s_ns.dev[idx].dev_eui;
            esp_err_t err = ESP_OK;
            memset(&s_stat[idx], 0, sizeof s_stat[idx]);
            if (lns_tx()) {
                handled = true;
                /* Store the DevNonce and JoinNonce before anything goes out. */
                err = save_dev(idx);
                if (err == ESP_OK) {
                    err = save_meta();
                }
                if (err == ESP_OK) {
                    err = save_cnt(idx);
                }
            } else {
                /* Dry run: nothing is sent, so nothing is stored, and in hybrid
                 * mode the join still goes to TTN, where the device may also be
                 * registered. The session exists in RAM only. */
                s_stat[idx].join_unanswered = true;
            }
            atomic_fetch_add(&s_joins, 1U);
            ESP_LOGI(TAG, "join: %s (%s) DevAddr %08" PRIX32 ", RSSI %d SNR %.1f", eui_str(eui, e),
                     s_ns.dev[idx].name, s_ns.dev[idx].dev_addr, p->rssi, (double)p->lsnr);
            xSemaphoreGive(s_lock);
            if (err == ESP_OK) {
                /* the device learns its windows from this frame: it uses the defaults for it */
                send_down(p, acc, alen, LWC_NS_JOIN_DELAY_US, LWC_NS_RX2_FREQ_HZ, 0, "join-accept", eui);
            } else {
                ESP_LOGE(TAG, "join not answered: storing the device failed (%s)", esp_err_to_name(err));
            }
            memset(acc, 0, sizeof acc);
            return handled;
        }
        handled = r == LWC_NS_JOIN_REPLAY;
        if (r == LWC_NS_JOIN_REPLAY || r == LWC_NS_JOIN_BAD_MIC) {
            lwc_lw_join_req_t jr;
            (void)lwc_lw_join_req_parse(p->payload, p->size, &jr);
            ESP_LOGW(TAG, "join from %s refused: %s", eui_str(jr.dev_eui, e),
                     r == LWC_NS_JOIN_REPLAY ? "DevNonce already used" : "MIC (wrong AppKey?)");
        }
    } else if (mt == LWC_LW_UNCONFIRMED_UP || mt == LWC_LW_CONFIRMED_UP) {
        const lwc_ns_up_res_t r = lwc_ns_uplink(&s_ns, p->payload, p->size, &up);
        if (r == LWC_NS_UP_OK || r == LWC_NS_UP_RETRANS) {
            handled = true;
            const char *who = s_ns.dev[up.idx].name[0] ? s_ns.dev[up.idx].name : eui_str(s_ns.dev[up.idx].dev_eui, e);
            if (r == LWC_NS_UP_OK) {
                dev_stat_t *st = &s_stat[up.idx];
                st->seen_us = esp_timer_get_time();
                st->uplinks++;
                st->rssi = p->rssi;
                st->snr = p->lsnr;
                st->sf = p->sf;
                st->has_port = up.has_port;
                st->port = up.port;
                st->len = (uint8_t)(up.len < PAYLOAD_SHOW ? up.len : PAYLOAD_SHOW);
                memcpy(st->payload, up.payload, st->len);
                atomic_fetch_add(&s_uplinks, 1U);
                ESP_LOGI(TAG, "uplink %s FCnt %" PRIu32 " port %u, %u B, RSSI %d SNR %.1f%s", who, up.fcnt,
                         up.port, (unsigned)up.len, p->rssi, (double)p->lsnr, up.confirmed ? " (confirmed)" : "");
            } else {
                ESP_LOGI(TAG, "uplink %s FCnt %" PRIu32 " repeated%s", who, up.fcnt,
                         up.confirmed ? " (ACK missed?)" : "");
            }
            bool send = answer(p, r, &up, &dl);
            /* One write for both counters; the downlink counter is stored
             * before the frame goes out, so it is never reused. */
            if (r == LWC_NS_UP_OK || send) {
                const esp_err_t err = save_cnt(up.idx);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "counters not stored (%s)%s", esp_err_to_name(err), send ? ": answer not sent" : "");
                    send = false;
                }
            }
            const uint64_t eui = s_ns.dev[up.idx].dev_eui;
            const uint32_t rx1_us = lwc_ns_rx1_delay_us(&s_ns.dev[up.idx]);
            const uint32_t rx2_hz = s_ns.dev[up.idx].rx2_hz != 0U ? s_ns.dev[up.idx].rx2_hz : LWC_NS_RX2_FREQ_HZ;
            const uint8_t rx2_dr = s_ns.dev[up.idx].rx2_dr;
            memset(&up, 0, sizeof up);
            xSemaphoreGive(s_lock);
            if (send) {
                char what[128];
                describe(&dl, what, sizeof what);
                send_down(p, dl.frame, dl.len, rx1_us, rx2_hz, rx2_dr, what, eui);
            }
            return handled;
        } else if (r == LWC_NS_UP_REPLAY) {
            handled = true;
            ESP_LOGW(TAG, "uplink replay dropped (old frame counter)");
        } else if (r == LWC_NS_UP_BAD_MIC) {
            ESP_LOGW(TAG, "uplink with a local DevAddr failed the MIC%s",
                     gwmode() == LWC_MODE_HYBRID ? ": forwarded" : "");
        }
    }
    xSemaphoreGive(s_lock);
    return handled;
}

static void lns_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("lns", 30000);
    static radio_pkt_t p;
    const lwc_mode_t mode = gwmode();
    for (;;) {
        sv_beat(sv);
        if (xQueueReceive(s_radio_q, &p, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        const bool handled = handle(&p);
        if (s_fwd_q != NULL && lwc_ns_forward(mode, handled)) {
            if (xQueueSend(s_fwd_q, &p, 0) == pdTRUE) {
                atomic_fetch_add(&s_fwd, 1U);
            } else {
                atomic_fetch_add(&s_fwd_drop, 1U);
            }
        }
    }
}

esp_err_t lns_start(QueueHandle_t radio_q, QueueHandle_t *fwd_q)
{
    ESP_RETURN_ON_FALSE(fwd_q != NULL, ESP_ERR_INVALID_ARG, TAG, "fwd_q");
    *fwd_q = NULL;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock");
    load(); /* devices can be managed in every mode */
    if (!gwmode_local()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(radio_q != NULL, ESP_ERR_INVALID_STATE, TAG, "no radio: local server not started");
    s_radio_q = radio_q;
    if (gwmode() == LWC_MODE_HYBRID) {
        s_fwd_q = xQueueCreate(RADIO_QUEUE_LEN, sizeof(radio_pkt_t));
        ESP_RETURN_ON_FALSE(s_fwd_q != NULL, ESP_ERR_NO_MEM, TAG, "forward queue");
        *fwd_q = s_fwd_q;
    }
    ESP_LOGI(TAG, "local network server: %s mode, NetID %06X%s", gwmode_name(gwmode()),
             (unsigned)LWC_NS_NET_ID, lns_tx() ? "" : ", DRY RUN (local transmit off)");
    const BaseType_t ok = xTaskCreatePinnedToCore(lns_task, "lns", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    atomic_store(&s_running, true);
    return ESP_OK;
}

/* ---- device management (web API) ---------------------------------------------- */

lwc_ns_err_t lns_add(uint64_t dev_eui, uint64_t join_eui, const uint8_t app_key[16], lwc_ns_mac_t mac,
                     const char *name)
{
    if (s_lock == NULL) {
        return LWC_NS_ERR_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t idx = 0;
    lwc_ns_err_t r = lwc_ns_add(&s_ns, dev_eui, join_eui, app_key, mac, name, &idx);
    if (r == LWC_NS_OK) {
        if (save_dev(idx) != ESP_OK || save_cnt(idx) != ESP_OK) {
            (void)lwc_ns_remove(&s_ns, dev_eui);
            r = LWC_NS_ERR_FULL; /* storage full or failing */
        } else {
            memset(&s_stat[idx], 0, sizeof s_stat[idx]);
        }
    }
    xSemaphoreGive(s_lock);
    char e[17];
    if (r == LWC_NS_OK) {
        ESP_LOGI(TAG, "device %s added", eui_str(dev_eui, e)); /* never the key */
    }
    return r;
}

lwc_ns_err_t lns_remove(uint64_t dev_eui)
{
    if (s_lock == NULL) {
        return LWC_NS_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = lwc_ns_find(&s_ns, dev_eui);
    lwc_ns_err_t r = LWC_NS_ERR_NOT_FOUND;
    if (i >= 0) {
        r = lwc_ns_remove(&s_ns, dev_eui);
        memset(&s_stat[i], 0, sizeof s_stat[i]);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char k[4];
            key_of(k, 'd', (size_t)i);
            erase_key(h, k);
            key_of(k, 'c', (size_t)i);
            erase_key(h, k);
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_lock);
    char e[17];
    if (r == LWC_NS_OK) {
        ESP_LOGI(TAG, "device %s removed", eui_str(dev_eui, e));
    }
    return r;
}

static size_t put(char *out, size_t cap, size_t o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static size_t put(char *out, size_t cap, size_t o, const char *fmt, ...)
{
    if (o >= cap) {
        return o;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    return n < 0 ? o : (o + (size_t)n < cap ? o + (size_t)n : cap);
}

size_t lns_devices_json(char *out, size_t cap)
{
    if (cap == 0U) {
        return 0;
    }
    size_t o = put(out, cap, 0, "[");
    bool first = true;
    const int64_t now = esp_timer_get_time();
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
            const lwc_ns_dev_t *d = &s_ns.dev[i];
            const dev_stat_t *st = &s_stat[i];
            if (!d->used) {
                continue;
            }
            char dev[17], join[17], pl[2U * PAYLOAD_SHOW + 1U];
            lwc_hex_encode(st->payload, st->len, pl);
            /* names are printable ASCII (core); escape what JSON needs */
            char name[2U * LWC_NS_NAME_MAX + 1U];
            size_t k = 0;
            for (const char *c = d->name; *c != '\0' && k + 2U < sizeof name; c++) {
                if (*c == '"' || *c == '\\') {
                    name[k++] = '\\';
                }
                name[k++] = *c;
            }
            name[k] = '\0';
            o = put(out, cap, o,
                    "%s{\"dev_eui\":\"%s\",\"join_eui\":\"%s\",\"name\":\"%s\",\"mac\":\"%s\","
                    "\"joined\":%s,\"answered\":%s,\"dev_addr\":\"%08" PRIX32 "\",\"fcnt_up\":%" PRIu32 ",\"uplinks\":%" PRIu32
                    ",\"seen_s\":%" PRId64 ",\"rssi\":%d,\"snr\":%.1f,\"sf\":%u,\"port\":%d,\"payload\":\"%s\","
                    "\"battery\":%d,\"margin\":%d,\"tx_power\":%u,\"adr\":%s,\"rx1_delay_s\":%u,\"rx2_hz\":%" PRIu32
                    ",\"rx2_dr\":%u}",
                    first ? "" : ",", eui_str(d->dev_eui, dev), eui_str(d->join_eui, join), name,
                    d->mac == LWC_NS_MAC_1_0_4 ? "1.0.4" : "1.0.2", d->joined ? "true" : "false",
                    st->join_unanswered ? "false" : "true",
                    d->joined ? d->dev_addr : 0U, d->has_up ? d->fcnt_up : 0U, st->uplinks,
                    st->seen_us != 0 ? (now - st->seen_us) / 1000000LL : (int64_t)-1, st->rssi,
                    (double)st->snr, st->sf, st->has_port ? st->port : -1, pl,
                    s_ns.link[i].has_status ? (int)s_ns.link[i].battery : -1, (int)s_ns.link[i].margin,
                    s_ns.link[i].tx_power, s_ns.link[i].adr_pending ? "\"pending\"" : "null", (unsigned)d->rx1_delay_s,
                    d->rx2_hz, (unsigned)d->rx2_dr);
            first = false;
        }
        xSemaphoreGive(s_lock);
    }
    o = put(out, cap, o, "]");
    return o;
}

void lns_status_line(char *buf, size_t len)
{
    unsigned devs = 0, joined = 0;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
            devs += s_ns.dev[i].used ? 1U : 0U;
            joined += (s_ns.dev[i].used && s_ns.dev[i].joined) ? 1U : 0U;
        }
        xSemaphoreGive(s_lock);
    }
    snprintf(buf, len,
             "mode=%s local=%s devices=%u joined=%u joins=%u uplinks=%u downlinks=%u forwarded=%u fwd_dropped=%u tx=%s",
             gwmode_name(gwmode()), lns_running() ? "on" : "off", devs, joined, atomic_load(&s_joins),
             atomic_load(&s_uplinks), atomic_load(&s_downs), atomic_load(&s_fwd), atomic_load(&s_fwd_drop),
             lns_tx() ? "on" : "dry-run");
}
