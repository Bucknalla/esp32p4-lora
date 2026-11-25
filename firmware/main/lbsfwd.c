/*
 * LoRa Basics Station LNS client (ADR-0016).
 *
 * One TLS WebSocket to wss://<host>:<port>/traffic/eui-<gateway EUI>, the API
 * key in the upgrade request's Authorization header. On connect the gateway
 * sends `version`; the server answers with `router_config` (checked against
 * the radio's EU868 plan, logged on a mismatch; the radio is not changed).
 *
 * Up: CRC-ok frames as jreq/updf/propdf, timed with xtime (core lwc/lbs.h).
 * Down: dnmsg to the radio task (RADIO_SRC_LBS); when the frame goes to the
 * radio, dntxed. The xtime clock is fed with the radio's counter once a
 * minute, and its session changes whenever the radio restarts.
 *
 * The API key is never logged: the WebSocket, transport and TLS components
 * log the request headers at debug level, so their levels are held at WARN.
 *
 * Two tasks touch the shared state: this client task (uplinks, results,
 * clock feed) and the WebSocket task (the event handler: messages from the
 * server). s_lock guards the clock, the session and the pending downlinks.
 */
#include "lbsfwd.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "conc_hal.h"
#include "radioplan.h"
#include "eth.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gwmode.h"
#include "lwc/lbs.h"
#include "radio_pkt.h"
#include "radio_tx.h"
#include "sdkconfig.h"
#include "sv.h"
#include "wifi.h"

static const char *TAG = "lbs";

#if CONFIG_GW_CONC_SX1302
#define MODEL "esp32p4-sx1302"
#else
#define MODEL "esp32p4-sx1301"
#endif

#define TASK_STACK 6144
#define TASK_PRIO 5
#define TASK_CORE 0
#define WS_TASK_STACK 7168
#define WS_BUFFER 2048         /* per WebSocket read; longer messages come in parts */
#define MSG_MAX 16384          /* largest message reassembled (router_config is a few KB) */
#define OUT_MAX 1400           /* one uplink message: 255-byte frame as hex plus metadata */
#define PENDING 8              /* downlinks waiting for their dntxed */
#define CLOCK_FEED_MS 60000    /* xtime clock: well inside its 35-minute limit */
#define CLOCK_WAIT_S 120       /* wait this long for SNTP before connecting (TLS checks dates) */
#define WALLCLOCK_VALID_S 1700000000LL
#define SV_BUDGET_MS 30000
#define RETRY_MIN_MS 10000     /* reconnect after a failure, doubling up to RETRY_MAX_MS */
#define RETRY_MAX_MS 300000
#define PENDING_MAX_US 15000000LL /* a downlink not on air by then was dropped (RX2 is at most ~7 s out) */

typedef struct {
    bool used;
    int64_t at_us;   /* when it was queued (esp_timer) */
    uint16_t token;
    int64_t diid;
    int64_t rctx;
    char dev_eui[LWC_LBS_EUI_STR];
} pending_t;

static QueueHandle_t s_queue;
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_lock;
static atomic_bool s_running, s_connected, s_up, s_plan_ok = true;
static int s_retry_ms = RETRY_MIN_MS; /* WebSocket task only */
static atomic_uint s_ups, s_downs, s_txed, s_refused, s_dropped;
static char s_host[GW_LBS_HOST_MAX];

/* guarded by s_lock */
static lwc_lbs_clock_t s_clk;
static uint8_t s_sess;
static unsigned s_starts = UINT32_MAX;
static pending_t s_pend[PENDING];
static uint16_t s_token;

/* WebSocket task only */
static char *s_msg;
static size_t s_msg_len;
static bool s_msg_skip;
static radio_tx_req_t s_req;

/* client task only */
static char s_out[OUT_MAX];

static void quiet_transport_logs(void)
{
    /* the TAGs of esp_websocket_client 1.8 and ESP-IDF 6.1 tcp_transport/esp-tls (debug output
     * is compiled out at CONFIG_LOG_MAXIMUM_LEVEL INFO anyway; this holds if that is raised) */
    static const char *const warn[] = {"websocket_client", "transport_ws", "transport_proxy"};
    for (size_t i = 0; i < sizeof warn / sizeof warn[0]; i++) {
        esp_log_level_set(warn[i], ESP_LOG_WARN);
    }
    /* esp-tls and the transport base log three lines on every failed retry;
     * this file and the MQTT client (mqttc.c) log each failure themselves,
     * with the esp-tls and mbedTLS codes. Same levels in both places. */
    static const char *const none[] = {"transport_base", "transport", "esp-tls", "esp-tls-mbedtls"};
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        esp_log_level_set(none[i], ESP_LOG_NONE);
    }
}

static int64_t unix_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < WALLCLOCK_VALID_S) {
        return -1;
    }
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* A new xtime session whenever the radio has (re)started. Call with s_lock. */
static void check_session(void)
{
    const unsigned starts = atomic_load(&g_radio_stats.starts);
    if (starts == s_starts) {
        return;
    }
    s_starts = starts;
    s_sess = (uint8_t)(1U + esp_random() % 255U);
    lwc_lbs_clock_init(&s_clk, s_sess);
    memset(s_pend, 0, sizeof s_pend); /* their xtimes belong to the old session */
    ESP_LOGI(TAG, "xtime session %u (radio start %u)", (unsigned)s_sess, starts);
}

/* ---- from the server (WebSocket task) ---------------------------------------------- */

static void send_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char v[256];
    size_t n = 0;
    if (lwc_lbs_version(v, sizeof v, "lwc-gateway", app->version, app->version, MODEL, &n) !=
            LWC_LBS_OK ||
        esp_websocket_client_send_text(s_ws, v, (int)n, pdMS_TO_TICKS(2000)) < 0) {
        ESP_LOGW(TAG, "sending version failed");
    }
}

static void on_router_config(const char *json, size_t len)
{
    lwc_lbs_router_config_t c;
    const lwc_lbs_err_t e = lwc_lbs_parse_router_config(json, len, &c);
    if (e != LWC_LBS_OK) {
        ESP_LOGW(TAG, "router_config not understood (%d)", (int)e);
        return;
    }
    const lwc_region_t *rg = radioplan_region();
    const bool drs_ok = lwc_lbs_router_drs_ok(&c, rg);
    ESP_LOGI(TAG, "router_config: region %s, %" PRIu32 "-%" PRIu32 " Hz, data rates %s %s, channels 0x%02X",
             c.region[0] != '\0' ? c.region : "-", c.freq_min, c.freq_max, drs_ok ? "match" : "DO NOT MATCH",
             rg->name, (unsigned)c.multisf);
    /* ADR-0017: the server's channel plan (frequencies and channels only) becomes the
     * radio's, if it is valid for the gateway's region (ADR-0024: its data rates and
     * default channels); the radio restarts only when it differs. */
    lwc_plan_t plan;
    bool ok = drs_ok && lwc_lbs_router_plan(&c, rg, radioplan_card(), &plan);
    if (!ok) {
        ESP_LOGW(TAG, "the server's channel plan is not usable here (%s checks): keeping the radio's", rg->name);
    } else {
        bool changed = false;
        const esp_err_t err = radioplan_store(&plan, RADIOPLAN_SERVER, &changed);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "the server's channel plan failed on this radio before: keeping the radio's");
            ok = false;
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "storing the server's channel plan failed (%s)", esp_err_to_name(err));
            ok = false;
        } else if (changed) {
            ESP_LOGW(TAG, "channel plan from the server: restarting the radio");
            conc_hal_restart();
        }
    }
    atomic_store(&s_plan_ok, ok);
    if (s_retry_ms != RETRY_MIN_MS) {
        s_retry_ms = RETRY_MIN_MS;
        (void)esp_websocket_client_set_reconnect_timeout(s_ws, s_retry_ms);
    }
    if (!atomic_exchange(&s_up, true)) {
        ESP_LOGI(TAG, "link up: %s", s_host);
    }
}

static void on_dnmsg(const char *json, size_t len)
{
    static lwc_lbs_dnmsg_t d;
    lwc_lbs_err_t e = lwc_lbs_parse_dnmsg(json, len, &d);
    if (e != LWC_LBS_OK) {
        ESP_LOGW(TAG, "dnmsg refused: %s", e == LWC_LBS_ERR_UNSUPPORTED ? "class B (no GPS) or no time to answer" : "malformed");
        atomic_fetch_add(&s_refused, 1U);
        return;
    }
    static lwc_txpk_t alt;
    memset(&s_req, 0, sizeof s_req);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    check_session();
    if (d.asap) {
        /* class C "immediately" (ChirpStack): the next free time on RX2, from the counter */
        e = atomic_load(&g_radio_counter_ok)
                ? lwc_lbs_dn_txpk_now(&d, radioplan_region(), (uint32_t)atomic_load(&g_radio_counter), &s_req.pk, &alt)
                : LWC_LBS_ERR_SESSION;
    } else {
        e = lwc_lbs_dn_txpk(&d, radioplan_region(), s_sess, &s_req.pk, &alt);
    }
    size_t slot = PENDING;
    if (e == LWC_LBS_OK) {
        for (size_t i = 0; i < PENDING; i++) {
            if (!s_pend[i].used) {
                slot = i;
                break;
            }
        }
        if (slot == PENDING) {
            slot = 0; /* full: the oldest results were lost; reuse */
        }
        s_req.token = ++s_token;
        s_pend[slot] = (pending_t){true, esp_timer_get_time(), s_req.token, d.diid, d.rctx, {0}};
        memcpy(s_pend[slot].dev_eui, d.dev_eui, sizeof d.dev_eui);
    }
    xSemaphoreGive(s_lock);
    if (e != LWC_LBS_OK) {
        ESP_LOGW(TAG, "dnmsg diid %" PRId64 " refused: %s", d.diid,
                 e == LWC_LBS_ERR_SESSION ? (d.asap ? "the radio is restarting" : "xtime from an earlier radio session")
                                          : "bad window");
        atomic_fetch_add(&s_refused, 1U);
        return;
    }
    s_req.source = RADIO_SRC_LBS;
    if (alt.size != 0U) {
        s_req.alt = (radio_tx_alt_t){true, alt.tmst, alt.freq_hz, alt.sf, alt.bw_khz};
    }
    if (g_radio_tx_req == NULL || xQueueSend(g_radio_tx_req, &s_req, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "dnmsg diid %" PRId64 ": radio queue full", d.diid);
        atomic_fetch_add(&s_refused, 1U);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pend[slot].used = false;
        xSemaphoreGive(s_lock);
        return;
    }
    atomic_fetch_add(&s_downs, 1U);
    ESP_LOGI(TAG, "downlink diid %" PRId64 " (token %u): %" PRIu32 " Hz SF%u, %u B%s", d.diid, (unsigned)s_req.token,
             s_req.pk.freq_hz, s_req.pk.sf, s_req.pk.size,
             d.asap ? ", class C now (second try 1 s later)" : s_req.alt.on ? ", RX2 as fallback" : "");
}

static void on_message(const char *json, size_t len)
{
    lwc_lbs_msg_t t;
    if (lwc_lbs_msgtype(json, len, &t) != LWC_LBS_OK) {
        ESP_LOGW(TAG, "message from the server not understood (%u B)", (unsigned)len);
        return;
    }
    switch (t) {
    case LWC_LBS_MSG_ROUTER_CONFIG:
        on_router_config(json, len);
        break;
    case LWC_LBS_MSG_DNMSG:
        on_dnmsg(json, len);
        break;
    case LWC_LBS_MSG_DNSCHED:
        ESP_LOGW(TAG, "dnsched ignored (class B: no GPS)");
        break;
    default:
        break; /* timesync and anything else */
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        atomic_store(&s_connected, true);
        ESP_LOGI(TAG, "connected to %s: sending version", s_host);
        send_version();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        atomic_store(&s_connected, false);
        if (atomic_exchange(&s_up, false)) {
            ESP_LOGW(TAG, "link down");
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        /* back off: 10 s, doubling to 5 min; a refused key goes to 5 min at once */
        s_retry_ms = (ev != NULL && (ev->error_handle.esp_ws_handshake_status_code == 401 ||
                                     ev->error_handle.esp_ws_handshake_status_code == 403))
                         ? RETRY_MAX_MS
                         : (s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2);
        (void)esp_websocket_client_set_reconnect_timeout(s_ws, s_retry_ms);
        if (ev != NULL && ev->error_handle.esp_ws_handshake_status_code != 0) {
            const int st = ev->error_handle.esp_ws_handshake_status_code;
            ESP_LOGE(TAG, "server refused the connection: HTTP %d%s; retry in %d s", st,
                     st == 401 || st == 403 ? " (check the LNS API key and its rights)" : "", s_retry_ms / 1000);
        } else if (ev != NULL) {
            /* the transport layers are silent (mqttc.c): their detail is here */
            ESP_LOGW(TAG, "connection error (tls 0x%x, mbedTLS -0x%04x, certificate flags 0x%x, socket errno %d); "
                          "retry in %d s",
                     ev->error_handle.esp_tls_last_esp_err,
                     (unsigned)(ev->error_handle.esp_tls_stack_err < 0 ? -ev->error_handle.esp_tls_stack_err
                                                                       : ev->error_handle.esp_tls_stack_err),
                     (unsigned)ev->error_handle.esp_tls_cert_verify_flags, ev->error_handle.esp_transport_sock_errno,
                     s_retry_ms / 1000);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev == NULL || (ev->op_code != 0x1 && ev->op_code != 0x0) || ev->data_len < 0) {
            break; /* binary, ping, pong, close */
        }
        if (ev->op_code == 0x1 && ev->payload_offset == 0) {
            s_msg_len = 0;
            s_msg_skip = false;
        }
        if (s_msg_skip || s_msg_len + (size_t)ev->data_len > MSG_MAX) {
            if (!s_msg_skip) {
                ESP_LOGW(TAG, "message over %u B dropped", (unsigned)MSG_MAX);
            }
            s_msg_skip = true;
        } else {
            memcpy(&s_msg[s_msg_len], ev->data_ptr, (size_t)ev->data_len);
            s_msg_len += (size_t)ev->data_len;
        }
        if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
            if (!s_msg_skip) {
                on_message(s_msg, s_msg_len);
            }
            s_msg_len = 0;
            s_msg_skip = false;
        }
        break;
    default:
        break;
    }
}

/* ---- to the server (client task) ---------------------------------------------------- */

static void send_up(const radio_pkt_t *p)
{
    if (p->stat != 1) {
        return; /* CRC bad or none: Station forwards CRC-ok frames only */
    }
    if (!atomic_load(&s_up)) {
        atomic_fetch_add(&s_dropped, 1U);
        return;
    }
    lwc_lbs_rx_t rx = {.sf = p->sf, .bw_khz = p->bw_khz, .freq_hz = p->freq_hz, .rssi = (float)p->rssi,
                       .snr = p->lsnr, .unix_us = p->time_unix_us};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    check_session();
    rx.xtime = lwc_lbs_xtime(&s_clk, p->tmst);
    xSemaphoreGive(s_lock);
    size_t n = 0;
    const lwc_lbs_err_t e = lwc_lbs_uplink(s_out, sizeof s_out, radioplan_region(), p->payload, p->size, &rx, &n);
    if (e != LWC_LBS_OK) {
        if (e != LWC_LBS_ERR_FRAME) {
            ESP_LOGW(TAG, "uplink not sent (%d)", (int)e);
        }
        return;
    }
    if (esp_websocket_client_send_text(s_ws, s_out, (int)n, pdMS_TO_TICKS(2000)) < 0) {
        atomic_fetch_add(&s_dropped, 1U);
        return;
    }
    atomic_fetch_add(&s_ups, 1U);
}

static void handle_results(void)
{
    /* downlinks the radio dropped without a result (missed their start) */
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < PENDING; i++) {
        if (s_pend[i].used && now - s_pend[i].at_us > PENDING_MAX_US) {
            s_pend[i].used = false;
        }
    }
    xSemaphoreGive(s_lock);
    radio_tx_res_t r;
    while (g_radio_tx_res != NULL && xQueueReceive(g_radio_tx_res, &r, 0) == pdTRUE) {
        pending_t p = {0};
        int64_t xt = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < PENDING; i++) {
            if (s_pend[i].used && s_pend[i].token == r.token) {
                if (r.on_air || r.result != RADIO_TX_OK) {
                    p = s_pend[i];
                    s_pend[i].used = false;
                }
                break;
            }
        }
        if (r.on_air && p.used) {
            xt = lwc_lbs_xtime(&s_clk, r.tmst);
        }
        xSemaphoreGive(s_lock);
        if (!p.used) {
            continue; /* admitted (on_air follows), or from an earlier session */
        }
        if (!r.on_air) {
            ESP_LOGW(TAG, "downlink diid %" PRId64 " not sent: %s", p.diid, radio_tx_result_str(r.result));
            atomic_fetch_add(&s_refused, 1U);
            continue;
        }
        size_t n = 0;
        if (lwc_lbs_dntxed(s_out, sizeof s_out, p.diid, p.dev_eui, p.rctx, xt, unix_us(), &n) == LWC_LBS_OK &&
            esp_websocket_client_send_text(s_ws, s_out, (int)n, pdMS_TO_TICKS(2000)) >= 0) {
            atomic_fetch_add(&s_txed, 1U);
        }
    }
}

static void feed_clock(void)
{
    if (!atomic_load(&g_radio_counter_ok)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    check_session();
    (void)lwc_lbs_xtime(&s_clk, atomic_load(&g_radio_counter));
    xSemaphoreGive(s_lock);
}

static bool have_ip(void)
{
    return wifi_has_ip() || eth_has_ip();
}

static void lbs_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("lbs", SV_BUDGET_MS);
    radio_pkt_t pkt;
    /* TLS checks certificate dates: wait for SNTP (bounded), dropping frames meanwhile */
    for (int64_t t0 = esp_timer_get_time();
         !have_ip() || (unix_us() < 0 && esp_timer_get_time() - t0 < CLOCK_WAIT_S * 1000000LL);) {
        sv_beat(sv);
        if (xQueueReceive(s_queue, &pkt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            atomic_fetch_add(&s_dropped, 1U);
        }
    }
    if (unix_us() < 0) {
        ESP_LOGW(TAG, "no wall clock yet: connecting anyway (certificate dates may fail)");
    }

    char eui[17];
    char key[GW_LBS_KEY_MAX];
    static char uri[GW_LBS_HOST_MAX + 64];
    static char hdr[GW_LBS_KEY_MAX + 32];
    const bool keyed = gwmode_lbs_key(key) == ESP_OK && key[0] != '\0';
    if (!keyed || gwmode_eui(eui) != ESP_OK) {
        ESP_LOGE(TAG, "%s: Basics Station link not started", keyed ? "no gateway EUI" : "no LNS API key set");
        memset(key, 0, sizeof key);
        for (;;) {
            sv_beat(sv);
            if (xQueueReceive(s_queue, &pkt, pdMS_TO_TICKS(1000)) == pdTRUE) {
                atomic_fetch_add(&s_dropped, 1U);
            }
        }
    }
    const uint16_t port = gwmode_lbs_port();
    snprintf(uri, sizeof uri, "wss://%s:%u/traffic/eui-%s", s_host, (unsigned)port, eui);
    snprintf(hdr, sizeof hdr, "Authorization: %s%s\r\n", strncmp(key, "Bearer ", 7) == 0 ? "" : "Bearer ", key);
    memset(key, 0, sizeof key);

    quiet_transport_logs();
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .headers = hdr,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = WS_BUFFER,
        .task_stack = WS_TASK_STACK,
        .task_prio = TASK_PRIO,
        .task_core_id_set = true,
        .task_core_id = TASK_CORE,
        .reconnect_timeout_ms = RETRY_MIN_MS,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 120,
        .enable_close_reconnect = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    memset(hdr, 0, sizeof hdr); /* the client keeps its own copy */
    if (s_ws == NULL ||
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL) != ESP_OK ||
        esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket client start failed");
    } else {
        ESP_LOGI(TAG, "connecting to wss://%s:%u (gateway EUI from the base MAC)", s_host, (unsigned)port);
    }

    int64_t next_feed = 0;
    for (;;) {
        sv_beat(sv);
        if (xQueueReceive(s_queue, &pkt, pdMS_TO_TICKS(200)) == pdTRUE) {
            send_up(&pkt);
        }
        handle_results();
        const int64_t now = esp_timer_get_time();
        if (now >= next_feed) {
            feed_clock();
            next_feed = now + (int64_t)CLOCK_FEED_MS * 1000LL;
        }
    }
}

esp_err_t lbsfwd_start(QueueHandle_t rx_queue)
{
    ESP_RETURN_ON_FALSE(rx_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "queue");
    s_queue = rx_queue;
    s_lock = xSemaphoreCreateMutex();
    s_msg = heap_caps_malloc(MSG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_lock != NULL && s_msg != NULL, ESP_ERR_NO_MEM, TAG, "memory");
    gwmode_lbs_host(s_host);
    atomic_store(&s_running, true);
    const BaseType_t ok = xTaskCreatePinnedToCore(lbs_task, "lbs", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool lbsfwd_link_up(void)
{
    return atomic_load(&s_up);
}

void lbsfwd_status(char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return;
    }
    if (!atomic_load(&s_running)) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "LBS host=%s link=%s plan=%s uplinks=%u downlinks=%u dntxed=%u refused=%u dropped=%u",
             s_host, atomic_load(&s_up) ? "up" : atomic_load(&s_connected) ? "connecting" : "down",
             atomic_load(&s_plan_ok) ? "ok" : "differs", atomic_load(&s_ups), atomic_load(&s_downs),
             atomic_load(&s_txed), atomic_load(&s_refused), atomic_load(&s_dropped));
}
