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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gwmode.h"
#include "lwc/dlq.h"
#include "lwc/hex.h"
#include "lwc/lorawan.h"
#include "lwc/mac.h"
#include "lwc/mqtt.h"
#include "nvs.h"
#include "radio_pkt.h"
#include "radio_tx.h"
#include "sdkconfig.h"
#include "codecs.h"
#include "mqttc.h"
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
#define DEV_VER 3U  /* device record: 3 adds the class (ADR-0020), 2 the receive windows (ADR-0017); older are migrated */
#define FCNT_DOWN_SKIP 16U /* downlink counter skip at boot: never reuse one */
#define DOWN_POWER_DBM 14  /* EU868 RX1 default; the radio caps it (ADR-0007) */
#define PAYLOAD_SHOW 32U
/* Class C (ADR-0020) */
#define CLASS_C_LEAD_US 300000U       /* a class C frame starts this long after the counter read */
#define CLASS_C_WINDOW_MS 1500        /* after RX2 opens: its frame may still be arriving */
#define LOOP_MS 100                   /* lns task: class C service period */

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

/* Device record version 2, frozen: lwc_ns_dev_t before the class (ADR-0020).
 * The class byte may land in tail padding, so versions are told apart by
 * `ver`, not by length alone. */
typedef struct {
    dev_v1_t v1;
    uint8_t rx1_delay_s;
    uint8_t rx2_dr;
    uint32_t rx2_hz;
} dev_v2_t;

typedef struct {
    uint8_t ver;
    dev_v2_t d;
} dev_rec_v2_t;

_Static_assert(offsetof(dev_rec_t, d) == offsetof(dev_rec_v2_t, d), "record header");
_Static_assert(offsetof(lwc_ns_dev_t, rx1_delay_s) == offsetof(dev_v2_t, rx1_delay_s), "v2 layout");
_Static_assert(offsetof(lwc_ns_dev_t, rx2_hz) == offsetof(dev_v2_t, rx2_hz), "v2 layout");

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

static void from_v2(const dev_v2_t *o, lwc_ns_dev_t *d)
{
    from_v1(&o->v1, d);
    d->rx1_delay_s = o->rx1_delay_s;
    d->rx2_dr = o->rx2_dr;
    d->rx2_hz = o->rx2_hz;
    d->dev_class = LWC_NS_CLASS_A; /* version 2 devices were class A */
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
/* the payload codec of each device slot (ADR-0021), "" for none; NVS key x<slot> */
static char s_codec[LWC_NS_MAX_DEVICES][CODEC_NAME_MAX + 1U];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_radio_q;
static QueueHandle_t s_fwd_q;
static atomic_bool s_running;
static atomic_uint s_joins, s_uplinks, s_downs, s_fwd, s_fwd_drop;
static uint16_t s_tx_token;

/* Application downlinks from MQTT (ADR-0018): a few per device, in PSRAM, sent
 * with the next class A answer (core/src/dlq.c has the rules). Guarded by
 * s_lock. */
static lwc_dlq_t *s_dlq; /* [LWC_NS_MAX_DEVICES] */
static atomic_uint s_app_queued, s_app_sent, s_app_dropped, s_app_acked, s_app_nacked;

/* The radio's answer for each device's application downlink (its queue head
 * is the only one on its way): written by the TX task without waiting
 * (lns_tx_result), applied to the queue by this task under s_lock
 * (txres_apply). token << 2 | 1 handed over, | 2 refused; 0 none. */
static _Atomic uint32_t s_txres[LWC_NS_MAX_DEVICES];

/* Application downlinks on their way to the radio, by request token: when the
 * TX task hands one to the concentrator, its "sent" result is published.
 * Written before the request is queued, so a fast answer always finds it. */
#define INFLIGHT_N 8
typedef struct {
    uint16_t token;
    uint8_t idx; /* device slot */
    mqttc_res_t res;
} inflight_t;
static inflight_t s_infl[INFLIGHT_N];
static unsigned s_infl_pos;
static portMUX_TYPE s_infl_mux = portMUX_INITIALIZER_UNLOCKED;

static void infl_put(uint16_t token, size_t idx, const mqttc_res_t *r)
{
    taskENTER_CRITICAL(&s_infl_mux);
    inflight_t *e = &s_infl[s_infl_pos++ % INFLIGHT_N];
    e->token = token;
    e->idx = (uint8_t)idx;
    e->res = *r;
    taskEXIT_CRITICAL(&s_infl_mux);
}

static bool infl_take(uint16_t token, size_t *idx, mqttc_res_t *out)
{
    bool found = false;
    taskENTER_CRITICAL(&s_infl_mux);
    for (size_t i = 0; i < INFLIGHT_N && token != 0U; i++) {
        if (s_infl[i].token == token) {
            if (out != NULL) {
                *out = s_infl[i].res;
            }
            if (idx != NULL) {
                *idx = s_infl[i].idx;
            }
            s_infl[i].token = 0;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_infl_mux);
    return found;
}

void lns_tx_result(uint16_t token, bool ok)
{
    mqttc_res_t r;
    size_t idx = 0;
    if (token == 0U || !infl_take(token, &idx, &r)) {
        return; /* a join-accept or a MAC-only answer */
    }
    if (idx < LWC_NS_MAX_DEVICES) {
        atomic_store(&s_txres[idx], ((uint32_t)token << 2) | (ok ? 1U : 2U));
    }
    if (ok) {
        atomic_fetch_add(&s_app_sent, 1U);
        mqttc_result(&r);
    }
}

/* Hand the radio's answer to the device's queue (s_lock held). */
static void txres_apply(size_t idx)
{
    const uint32_t v = atomic_exchange(&s_txres[idx], 0U);
    if (v != 0U && s_dlq != NULL) {
        lwc_dlq_tx_result(&s_dlq[idx], (uint16_t)(v >> 2), (v & 3U) == 1U);
    }
}

/* The "sent" result of a queued item. */
static void dl_result_fill(const lwc_dlq_item_t *it, mqttc_res_t *r)
{
    *r = (mqttc_res_t){.dev_eui = it->dev_eui,
                       .state = (uint8_t)LWC_MQTT_DL_SENT,
                       .port = it->port,
                       .len = it->len,
                       .confirmed = it->confirmed,
                       .has_fcnt = it->tx > 0U,
                       .fcnt_down = it->fcnt,
                       .tx = it->tx};
    memcpy(r->id, it->id, sizeof r->id);
}

/* A result for a queued item; `reason` is a string literal or NULL. */
static void dl_result(const lwc_dlq_item_t *it, lwc_mqtt_dl_state_t st, const char *reason)
{
    mqttc_res_t r;
    dl_result_fill(it, &r);
    r.state = (uint8_t)st;
    r.reason = reason;
    mqttc_result(&r);
}

/* Drop everything waiting for one device, with a result for each. */
static void dlq_drop_all(lwc_dlq_t *q, const char *reason)
{
    static lwc_dlq_item_t it; /* s_lock held */
    while (lwc_dlq_pop(q, &it)) {
        atomic_fetch_add(&s_app_dropped, 1U);
        dl_result(&it, LWC_MQTT_DL_DROPPED, reason);
    }
    memset(&it, 0, sizeof it);
}

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
        } else if (len == sizeof(dev_rec_v2_t) && r->ver == 2U && ((const dev_rec_v2_t *)r)->d.v1.used) {
            from_v2(&((const dev_rec_v2_t *)r)->d, &s_ns.dev[i]);
            migrate[i] = true;
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
        key_of(k, 'x', i);
        size_t cl = sizeof s_codec[i];
        if (nvs_get_str(h, k, s_codec[i], &cl) != ESP_OK || !codecs_name_ok(s_codec[i])) {
            s_codec[i][0] = '\0';
        }
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
            ESP_LOGW(TAG, "device %s: record migrated to version %u%s", s_ns.dev[i].name, DEV_VER,
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
/* Returns the radio request token, 0 when nothing was handed to the radio. */
static uint16_t send_down(const radio_pkt_t *p, const uint8_t *frame, size_t len, uint32_t delay_us,
                          uint32_t rx2_hz, uint8_t rx2_dr, const char *what, uint64_t dev_eui,
                          const mqttc_res_t *sent_res, size_t idx)
{
    static radio_tx_req_t req;
    memset(&req, 0, sizeof req);
    req.source = RADIO_SRC_LNS;
    req.token = ++s_tx_token;
    if (req.token == 0U) {
        req.token = ++s_tx_token; /* 0: no request */
    }
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
        return 0;
    }
    if (sent_res != NULL) {
        infl_put(req.token, idx, sent_res); /* before the TX task can answer */
    }
    if (g_radio_tx_req == NULL || xQueueSend(g_radio_tx_req, &req, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "%s for %s: radio queue full", what, eui_str(dev_eui, e));
        if (sent_res != NULL) {
            (void)infl_take(req.token, NULL, NULL);
        }
        return 0;
    }
    atomic_fetch_add(&s_downs, 1U);
    ESP_LOGI(TAG, "%s %u for %s: RX1 %" PRIu32 " Hz SF%u, queued %" PRId32 " ms after the uplink", what,
             (unsigned)req.token, eui_str(dev_eui, e), req.pk.freq_hz, req.pk.sf, late);
    return req.token;
}

/* After an accepted or repeated uplink: build the class A answer if the
 * device needs one (this advances the downlink counter). Called with s_lock
 * held; the caller stores the counters before sending. */
static bool answer(const radio_pkt_t *p, lwc_ns_up_res_t r, const lwc_ns_uplink_t *up, const lwc_ns_app_t *app,
                   lwc_ns_dl_t *dl)
{
    lwc_ns_rx_t rx = {
        .snr10 = (int16_t)lroundf(p->lsnr * 10.0f),
        .sf = p->sf,
        .bw_khz = p->bw_khz,
        .now_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
    };
    rx.time_ok = lwc_gps_from_unix_us(p->time_unix_us, &rx.gps_s, &rx.gps_frac);
    return lwc_ns_answer_app(&s_ns, r, up, &rx, app, dl);
}

static void describe(const lwc_ns_dl_t *dl, char *out, size_t cap)
{
    char adr[40] = "";
    if (dl->link_adr) {
        (void)snprintf(adr, sizeof adr, " LinkADRReq(DR%u, power -%u dB)", dl->adr_dr, 2U * dl->adr_power);
    }
    char app[32] = "";
    if (dl->app) {
        lwc_lw_data_t d;
        if (lwc_lw_data_parse(dl->frame, dl->len, &d) == LWC_LW_OK) {
            (void)snprintf(app, sizeof app, " port %u %u B%s", d.fport, (unsigned)d.frm_len,
                           (d.fctrl & LWC_LW_FCTRL_FPENDING) != 0U ? " FPending" : "");
        }
    }
    (void)snprintf(out, cap, "downlink FCnt %" PRIu32 "%s%s%s%s%s%s", dl->fcnt, dl->ack ? " ACK" : "",
                   dl->link_check ? " LinkCheckAns" : "", dl->device_time ? " DeviceTimeAns" : "", adr,
                   dl->dev_status ? " DevStatusReq" : "", app);
}

/* Returns true when the frame was handled here (not to be forwarded). */
/* Radio fields of an MQTT event (ADR-0018). */
static void ev_radio(mqttc_ev_t *ev, const radio_pkt_t *p, const lwc_ns_dev_t *d)
{
    memset(ev, 0, sizeof *ev);
    ev->dev_eui = d->dev_eui;
    memcpy(ev->name, d->name, sizeof ev->name);
    ev->name[sizeof ev->name - 1U] = '\0';
    ev->dev_addr = d->dev_addr;
    ev->rssi = p->rssi;
    ev->snr = p->lsnr;
    ev->sf = p->sf;
    ev->bw_khz = p->bw_khz;
    ev->freq_hz = p->freq_hz;
    ev->time_unix_s = p->time_unix_us >= 0 ? p->time_unix_us / 1000000 : -1;
    ev->port = -1;
    const size_t i = (size_t)(d - s_ns.dev);
    if (i < LWC_NS_MAX_DEVICES) {
        memcpy(ev->codec, s_codec[i], sizeof ev->codec);
    }
}

static bool handle(const radio_pkt_t *p)
{
    if (p->stat != 1) {
        return false;
    }
    static uint8_t acc[40];
    static mqttc_ev_t ev;
    bool post = false;
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
                 * mode the join still goes to the network server, where the device may also be
                 * registered. The session exists in RAM only. */
                s_stat[idx].join_unanswered = true;
            }
            atomic_fetch_add(&s_joins, 1U);
            ESP_LOGI(TAG, "join: %s (%s) DevAddr %08" PRIX32 ", RSSI %d SNR %.1f", eui_str(eui, e),
                     s_ns.dev[idx].name, s_ns.dev[idx].dev_addr, p->rssi, (double)p->lsnr);
            if (handled && err == ESP_OK) {
                ev_radio(&ev, p, &s_ns.dev[idx]);
                ev.kind = MQTTC_JOIN;
                post = true;
            }
            xSemaphoreGive(s_lock);
            if (err == ESP_OK) {
                /* the device learns its windows from this frame: it uses the defaults for it */
                (void)send_down(p, acc, alen, LWC_NS_JOIN_DELAY_US, LWC_NS_RX2_FREQ_HZ, 0, "join-accept", eui, NULL, 0);
                if (post) {
                    mqttc_post(&ev);
                }
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
            /* application downlink: settle the one that went out, offer the next */
            lwc_dlq_t *q = s_dlq != NULL ? &s_dlq[up.idx] : NULL;
            const uint64_t dev_eui = s_ns.dev[up.idx].dev_eui;
            static lwc_dlq_item_t done;
            if (q != NULL) {
                atomic_fetch_add(&s_app_dropped, lwc_dlq_own(q, dev_eui)); /* the slot has a new device */
                txres_apply(up.idx);
                switch (lwc_dlq_uplink(q, r == LWC_NS_UP_OK, up.ack, p->tmst, &done)) {
                case LWC_DLQ_ACKED:
                    atomic_fetch_add(&s_app_acked, 1U);
                    ESP_LOGI(TAG, "confirmed downlink for %s on port %u acknowledged", who, done.port);
                    dl_result(&done, LWC_MQTT_DL_ACKED, NULL);
                    break;
                case LWC_DLQ_NACKED:
                    atomic_fetch_add(&s_app_nacked, 1U);
                    ESP_LOGW(TAG, "confirmed downlink for %s on port %u not acknowledged", who, done.port);
                    dl_result(&done, LWC_MQTT_DL_NACKED, "no ACK in the device's next uplink");
                    break;
                default: /* KEEP; DELIVERED: its "sent" was the last word */
                    break;
                }
                if (q->n > 0U && !lns_tx()) {
                    /* local transmit was switched off after they were queued: never sent late */
                    ESP_LOGW(TAG, "%u downlink(s) for %s dropped: local transmit is off", q->n, who);
                    dlq_drop_all(q, "local transmit is off");
                }
                /* class C: no send of its own while this uplink's class A windows are open */
                lwc_dlq_hold(q, esp_timer_get_time() / 1000 +
                                    ((int64_t)s_ns.dev[up.idx].rx1_delay_s + 1) * 1000 + CLASS_C_WINDOW_MS);
            }
            lwc_ns_app_t app;
            const lwc_dlq_item_t *head = lwc_dlq_head(q);
            if (head != NULL && head->sent) {
                head = NULL; /* class C, on its way: settled later, not offered twice */
            }
            if (head != NULL) {
                app = (lwc_ns_app_t){.port = head->port, .data = head->data, .len = head->len, .more = q->n > 1U,
                                     .confirmed = head->confirmed};
            }
            bool send = answer(p, r, &up, head != NULL ? &app : NULL, &dl);
            if (head != NULL && dl.app_too_big) {
                /* does not fit at this data rate: dropped, or it would block the queue */
                ESP_LOGW(TAG, "downlink for %s on port %u (%u B) dropped: too big for SF%u and RX2 DR%u", who,
                         head->port, head->len, p->sf, s_ns.dev[up.idx].rx2_dr);
                (void)lwc_dlq_pop(q, &done);
                atomic_fetch_add(&s_app_dropped, 1U);
                dl_result(&done, LWC_MQTT_DL_DROPPED, "too big for this uplink's data rate");
                head = NULL;
            }
            /* its "sent" result, filled now (the head may change once the lock is given) */
            static mqttc_res_t sent_res;
            if (send && dl.app && head != NULL) {
                lwc_dlq_item_t next = *head;
                next.tx++;
                next.fcnt = dl.fcnt;
                dl_result_fill(&next, &sent_res);
            }
            memset(&done, 0, sizeof done);
            /* One write for both counters; the downlink counter is stored
             * before the frame goes out, so it is never reused. */
            if (r == LWC_NS_UP_OK || send) {
                const esp_err_t err = save_cnt(up.idx);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "counters not stored (%s)%s", esp_err_to_name(err), send ? ": answer not sent" : "");
                    send = false;
                }
            }
            if (r == LWC_NS_UP_OK) {
                /* copied now: `up` is wiped below */
                ev_radio(&ev, p, &s_ns.dev[up.idx]);
                ev.kind = MQTTC_UP;
                ev.fcnt = up.fcnt;
                ev.confirmed = up.confirmed;
                ev.port = up.has_port ? (int16_t)up.port : -1;
                ev.len = (uint8_t)(up.len <= sizeof ev.data ? up.len : sizeof ev.data);
                memcpy(ev.data, up.payload, ev.len);
                post = true;
            }
            const uint64_t eui = dev_eui;
            const size_t up_idx = up.idx;
            const uint32_t rx1_us = lwc_ns_rx1_delay_us(&s_ns.dev[up.idx]);
            const uint32_t rx2_hz = s_ns.dev[up.idx].rx2_hz != 0U ? s_ns.dev[up.idx].rx2_hz : LWC_NS_RX2_FREQ_HZ;
            const uint8_t rx2_dr = s_ns.dev[up.idx].rx2_dr;
            memset(&up, 0, sizeof up);
            xSemaphoreGive(s_lock);
            if (send) {
                char what[160];
                describe(&dl, what, sizeof what);
                const uint16_t token = send_down(p, dl.frame, dl.len, rx1_us, rx2_hz, rx2_dr, what, eui,
                                                 dl.app ? &sent_res : NULL, up_idx);
                if (token != 0U && dl.app) {
                    /* on its way: settled by the next uplink. Only this task
                     * takes from the head; lns_remove may have emptied it. */
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    const lwc_dlq_item_t *h = lwc_dlq_head(&s_dlq[up_idx]);
                    if (h != NULL && h->dev_eui == eui) {
                        lwc_dlq_sent(&s_dlq[up_idx], token, dl.fcnt);
                    }
                    xSemaphoreGive(s_lock);
                }
            }
            if (post) {
                mqttc_post(&ev); /* after the answer is queued: never delays it */
                memset(ev.data, 0, sizeof ev.data);
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

/* Class C (ADR-0020): send a queued downlink at once on the device's RX2
 * frequency and data rate; settle it by the radio's answer, the ACK, or a
 * timeout (rules in core/src/dlq.c). Called every LOOP_MS by this task. */
static uint16_t send_c(size_t idx, const lwc_ns_dl_t *dl, uint32_t tmst, uint32_t freq_hz, uint8_t dr,
                       uint64_t dev_eui, const mqttc_res_t *sent_res)
{
    static radio_tx_req_t req;
    memset(&req, 0, sizeof req);
    req.source = RADIO_SRC_LNS;
    req.token = ++s_tx_token;
    if (req.token == 0U) {
        req.token = ++s_tx_token;
    }
    lwc_ns_downlink_at(tmst, freq_hz, dr, DOWN_POWER_DBM, dl->frame, dl->len, &req.pk);
    char e[17];
    infl_put(req.token, idx, sent_res); /* before the TX task can answer */
    if (g_radio_tx_req == NULL || xQueueSend(g_radio_tx_req, &req, pdMS_TO_TICKS(10)) != pdTRUE) {
        (void)infl_take(req.token, NULL, NULL);
        ESP_LOGW(TAG, "class C downlink for %s: radio queue full", eui_str(dev_eui, e));
        return 0;
    }
    atomic_fetch_add(&s_downs, 1U);
    ESP_LOGI(TAG, "class C downlink FCnt %" PRIu32 "%s %u for %s: %" PRIu32 " Hz DR%u, %u B", dl->fcnt,
             dl->confirmed ? " confirmed" : "", (unsigned)req.token, eui_str(dev_eui, e), freq_hz, dr,
             (unsigned)dl->len);
    return req.token;
}

static void class_c_service(void)
{
    if (s_dlq == NULL) {
        return;
    }
    static lwc_ns_dl_t dl;
    static lwc_dlq_item_t done;
    static mqttc_res_t sent_res;
    for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
        const lwc_ns_dev_t *d = &s_ns.dev[i];
        if (!d->used || d->dev_class != LWC_NS_CLASS_C || s_dlq[i].n == 0U) {
            continue; /* read without the lock: a stale view only delays by one pass */
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        lwc_dlq_t *q = &s_dlq[i];
        const int64_t now_ms = esp_timer_get_time() / 1000;
        atomic_fetch_add(&s_app_dropped, lwc_dlq_own(q, d->dev_eui));
        txres_apply(i);
        if (q->n > 0U && !lns_tx()) {
            dlq_drop_all(q, "local transmit is off");
        }
        const char *why = NULL;
        bool send = false;
        switch (lwc_dlq_tick(q, now_ms, (int64_t)lwc_ns_c_ack_s(d) * 1000, &done, &why)) {
        case LWC_DLQ_TIMEOUT:
            atomic_fetch_add(&s_app_nacked, 1U);
            ESP_LOGW(TAG, "class C confirmed downlink for %s on port %u: %s", d->name, done.port, why);
            dl_result(&done, LWC_MQTT_DL_NACKED, why);
            break;
        case LWC_DLQ_DROPPED:
            atomic_fetch_add(&s_app_dropped, 1U);
            ESP_LOGW(TAG, "class C downlink for %s on port %u dropped: %s", d->name, done.port, why);
            dl_result(&done, LWC_MQTT_DL_DROPPED, why);
            break;
        case LWC_DLQ_SEND:
            send = true;
            break;
        default: /* KEEP; DELIVERED: its "sent" was the last word */
            break;
        }
        uint32_t tmst = 0, end = 0;
        const lwc_dlq_item_t *h = lwc_dlq_head(q);
        if (send && h != NULL && !atomic_load(&g_radio_counter_ok)) {
            lwc_dlq_defer(q, now_ms + 1000); /* the radio is restarting */
            send = false;
        }
        if (send && h != NULL) {
            const lwc_ns_app_t app = {.port = h->port, .data = h->data, .len = h->len, .more = q->n > 1U,
                                      .confirmed = h->confirmed};
            if (lwc_ns_class_c_down(&s_ns, i, &app, &dl)) {
                if (save_cnt(i) == ESP_OK) { /* the counter, before the frame goes out */
                    tmst = (uint32_t)atomic_load(&g_radio_counter) + CLASS_C_LEAD_US;
                    end = tmst + lwc_lora_airtime_us((uint8_t)(12U - d->rx2_dr), 125, 5, 8, (uint16_t)dl.len,
                                                     false, false);
                    lwc_dlq_item_t next = *h;
                    next.tx++;
                    next.fcnt = dl.fcnt;
                    dl_result_fill(&next, &sent_res);
                } else {
                    lwc_dlq_defer(q, now_ms + LWC_DLQ_C_GAP_MS);
                    send = false;
                }
            } else if (dl.app_too_big) {
                (void)lwc_dlq_pop(q, &done);
                atomic_fetch_add(&s_app_dropped, 1U);
                dl_result(&done, LWC_MQTT_DL_DROPPED, "too big for the device's RX2 window");
                send = false;
            } else {
                lwc_dlq_defer(q, now_ms + LWC_DLQ_C_GAP_MS); /* not joined, or no uplink since the join */
                send = false;
            }
        }
        const uint64_t eui = d->dev_eui;
        const uint32_t rx2_hz = d->rx2_hz != 0U ? d->rx2_hz : LWC_NS_RX2_FREQ_HZ;
        const uint8_t rx2_dr = d->rx2_dr;
        xSemaphoreGive(s_lock);
        if (!send) {
            continue;
        }
        const uint16_t token = send_c(i, &dl, tmst, rx2_hz, rx2_dr, eui, &sent_res);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const lwc_dlq_item_t *h2 = lwc_dlq_head(q);
        if (h2 != NULL && h2->dev_eui == eui && !h2->sent) {
            if (token != 0U) {
                lwc_dlq_sent_c(q, token, dl.fcnt, now_ms, end);
            } else {
                lwc_dlq_defer(q, now_ms + LWC_DLQ_C_GAP_MS);
            }
        }
        xSemaphoreGive(s_lock);
        memset(&dl, 0, sizeof dl);
    }
    memset(&done, 0, sizeof done);
}

static void lns_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("lns", 30000);
    static radio_pkt_t p;
    const lwc_mode_t mode = gwmode();
    for (;;) {
        sv_beat(sv);
        class_c_service();
        if (xQueueReceive(s_radio_q, &p, pdMS_TO_TICKS(LOOP_MS)) != pdTRUE) {
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
    s_dlq = heap_caps_calloc(LWC_NS_MAX_DEVICES, sizeof(lwc_dlq_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_dlq != NULL, ESP_ERR_NO_MEM, TAG, "downlink queue");
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
                     const char *name, lwc_ns_class_t cls)
{
    if (s_lock == NULL) {
        return LWC_NS_ERR_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t idx = 0;
    lwc_ns_err_t r = lwc_ns_add(&s_ns, dev_eui, join_eui, app_key, mac, name, &idx);
    if (r == LWC_NS_OK) {
        (void)lwc_ns_set_class(&s_ns, dev_eui, cls);
        if (save_dev(idx) != ESP_OK || save_cnt(idx) != ESP_OK) {
            (void)lwc_ns_remove(&s_ns, dev_eui);
            r = LWC_NS_ERR_FULL; /* storage full or failing */
        } else {
            memset(&s_stat[idx], 0, sizeof s_stat[idx]);
            s_codec[idx][0] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
    char e[17];
    if (r == LWC_NS_OK) {
        ESP_LOGI(TAG, "device %s added", eui_str(dev_eui, e)); /* never the key */
    }
    return r;
}

lwc_ns_err_t lns_set_class(uint64_t dev_eui, const lwc_ns_class_t *cls, const uint16_t *c_ack_s)
{
    if (s_lock == NULL) {
        return LWC_NS_ERR_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = lwc_ns_find(&s_ns, dev_eui);
    lwc_ns_err_t r = i < 0 ? LWC_NS_ERR_NOT_FOUND : LWC_NS_OK;
    lwc_ns_dev_t was;
    if (r == LWC_NS_OK) {
        was = s_ns.dev[i];
        if (cls != NULL) {
            r = lwc_ns_set_class(&s_ns, dev_eui, *cls);
        }
        if (r == LWC_NS_OK && c_ack_s != NULL) {
            r = lwc_ns_set_c_ack(&s_ns, dev_eui, *c_ack_s);
        }
        if (r == LWC_NS_OK && save_dev((size_t)i) != ESP_OK) {
            r = LWC_NS_ERR_FULL;
        }
        if (r != LWC_NS_OK) {
            s_ns.dev[i].dev_class = was.dev_class;
            s_ns.dev[i].c_ack_s = was.c_ack_s;
        }
        memset(&was, 0, sizeof was); /* keys */
    }
    char e[17];
    if (r == LWC_NS_OK) {
        ESP_LOGI(TAG, "device %s: class %c, ACK wait %u s", eui_str(dev_eui, e),
                 s_ns.dev[i].dev_class == LWC_NS_CLASS_C ? 'C' : 'A', lwc_ns_c_ack_s(&s_ns.dev[i]));
    }
    xSemaphoreGive(s_lock);
    return r;
}

lwc_ns_err_t lns_set_codec(uint64_t dev_eui, const char *codec)
{
    if (s_lock == NULL || codec == NULL || (codec[0] != '\0' && !codecs_name_ok(codec))) {
        return LWC_NS_ERR_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = lwc_ns_find(&s_ns, dev_eui);
    lwc_ns_err_t r = i < 0 ? LWC_NS_ERR_NOT_FOUND : LWC_NS_OK;
    if (r == LWC_NS_OK) {
        nvs_handle_t h;
        char k[4];
        key_of(k, 'x', (size_t)i);
        esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            if (codec[0] != '\0') {
                err = nvs_set_str(h, k, codec);
            } else {
                err = nvs_erase_key(h, k);
                err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
            }
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            (void)snprintf(s_codec[i], sizeof s_codec[i], "%s", codec);
        } else {
            r = LWC_NS_ERR_FULL;
        }
    }
    xSemaphoreGive(s_lock);
    char e[17];
    if (r == LWC_NS_OK) {
        ESP_LOGI(TAG, "device %s: codec %s", eui_str(dev_eui, e), codec[0] != '\0' ? codec : "none");
    }
    return r;
}

bool lns_codec_of(uint64_t dev_eui, char out[CODEC_NAME_MAX + 1U])
{
    out[0] = '\0';
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    const int i = lwc_ns_find(&s_ns, dev_eui);
    if (i >= 0) {
        memcpy(out, s_codec[i], CODEC_NAME_MAX + 1U);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
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
        s_codec[i][0] = '\0';
        if (s_dlq != NULL) {
            dlq_drop_all(&s_dlq[i], "device removed");
        }
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char k[4];
            key_of(k, 'd', (size_t)i);
            erase_key(h, k);
            key_of(k, 'c', (size_t)i);
            erase_key(h, k);
            key_of(k, 'x', (size_t)i);
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
                    ",\"rx2_dr\":%u,\"class\":\"%c\",\"c_ack_s\":%u,\"waiting\":%u,\"codec\":\"%s\"}",
                    first ? "" : ",", eui_str(d->dev_eui, dev), eui_str(d->join_eui, join), name,
                    d->mac == LWC_NS_MAC_1_0_4 ? "1.0.4" : "1.0.2", d->joined ? "true" : "false",
                    st->join_unanswered ? "false" : "true",
                    d->joined ? d->dev_addr : 0U, d->has_up ? d->fcnt_up : 0U, st->uplinks,
                    st->seen_us != 0 ? (now - st->seen_us) / 1000000LL : (int64_t)-1, st->rssi,
                    (double)st->snr, st->sf, st->has_port ? st->port : -1, pl,
                    s_ns.link[i].has_status ? (int)s_ns.link[i].battery : -1, (int)s_ns.link[i].margin,
                    s_ns.link[i].tx_power, s_ns.link[i].adr_pending ? "\"pending\"" : "null", (unsigned)d->rx1_delay_s,
                    d->rx2_hz, (unsigned)d->rx2_dr, d->dev_class == LWC_NS_CLASS_C ? 'C' : 'A', lwc_ns_c_ack_s(d),
                    s_dlq != NULL ? (unsigned)s_dlq[i].n : 0U, s_codec[i]);
            first = false;
        }
        xSemaphoreGive(s_lock);
    }
    o = put(out, cap, o, "]");
    return o;
}

void lns_status_line(char *buf, size_t len)
{
    unsigned devs = 0, joined = 0, waiting = 0;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < LWC_NS_MAX_DEVICES; i++) {
            devs += s_ns.dev[i].used ? 1U : 0U;
            joined += (s_ns.dev[i].used && s_ns.dev[i].joined) ? 1U : 0U;
            waiting += s_dlq != NULL ? s_dlq[i].n : 0U;
        }
        xSemaphoreGive(s_lock);
    }
    snprintf(buf, len,
             "mode=%s local=%s devices=%u joined=%u joins=%u uplinks=%u downlinks=%u forwarded=%u fwd_dropped=%u tx=%s"
             " app_waiting=%u app_queued=%u app_sent=%u app_acked=%u app_nacked=%u app_dropped=%u",
             gwmode_name(gwmode()), lns_running() ? "on" : "off", devs, joined, atomic_load(&s_joins),
             atomic_load(&s_uplinks), atomic_load(&s_downs), atomic_load(&s_fwd), atomic_load(&s_fwd_drop),
             lns_tx() ? "on" : "dry-run", waiting, atomic_load(&s_app_queued), atomic_load(&s_app_sent),
             atomic_load(&s_app_acked), atomic_load(&s_app_nacked), atomic_load(&s_app_dropped));
}

esp_err_t lns_queue_down(uint64_t dev_eui, uint8_t port, const uint8_t *data, size_t len, bool confirmed,
                         const char *id)
{
    if (!lns_running() || s_dlq == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lns_tx()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (port < 1U || port > 223U || len > LWC_DLQ_DATA_MAX || (len > 0U && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    static lwc_dlq_item_t it; /* under s_lock */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    const int i = lwc_ns_find(&s_ns, dev_eui);
    if (i < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else if (len > lwc_ns_max_frm(s_ns.dev[i].rx2_dr)) {
        err = ESP_ERR_INVALID_SIZE; /* never fits the RX2 window */
    } else {
        lwc_dlq_t *q = &s_dlq[i];
        atomic_fetch_add(&s_app_dropped, lwc_dlq_own(q, dev_eui));
        memset(&it, 0, sizeof it);
        it.dev_eui = dev_eui;
        it.port = port;
        it.len = (uint8_t)len;
        it.confirmed = confirmed;
        if (id != NULL) {
            (void)snprintf(it.id, sizeof it.id, "%s", id);
        }
        if (len > 0U) {
            memcpy(it.data, data, len);
        }
        if (lwc_dlq_push(q, &it)) {
            atomic_fetch_add(&s_app_queued, 1U);
        } else {
            err = ESP_ERR_NO_MEM;
        }
        memset(&it, 0, sizeof it);
    }
    xSemaphoreGive(s_lock);
    return err;
}
