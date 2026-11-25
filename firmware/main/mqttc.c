/*
 * MQTT client (ADR-0018): uplink and join events out, downlinks in.
 *
 * The local network server posts events with mqttc_post(), which never
 * blocks: a zero-timeout send to a queue in PSRAM. This task turns them into
 * JSON (core/src/mqtt.c) and publishes them. Uplinks and joins go out at QoS 0
 * and only while connected: when the broker is away they are dropped and
 * counted, so memory stays bounded. The status is retained QoS 1; the
 * client's outbox is capped (OUTBOX_LIMIT, in PSRAM).
 *
 * On every connect: lora/<gw>/status "online" (the last will is "offline"),
 * and a subscription to lora/<gw>/+/down (clean session).
 *
 * Downlink results (lora/<gw>/<node>/down/result, QoS 1, not retained) come
 * through a second, smaller queue, so a flood of bad downlinks never takes
 * room from uplink events; refusals may fill only half of it, so results of
 * real downlinks keep room. Like events, they are dropped while the broker is
 * away.
 *
 * With TLS on (ADR-0019) the broker's certificate is checked against the
 * ESP-IDF CA bundle (public CAs, e.g. Let's Encrypt) and its host name.
 *
 * The password is kept in NVS and given only to the client; it is never
 * logged or returned by the API.
 */
#include "mqttc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gwmode.h"
#include "lns.h"
#include "lwc/mqtt.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "sv.h"

static const char *TAG = "mqtt";

#define NVS_NS "mqtt"
#define KEY_ON "on"
#define KEY_HOST "host"
#define KEY_PORT "port"
#define KEY_USER "user"
#define KEY_PASS "pass"
#define KEY_GWID "gwid"
#define KEY_TLS "tls"

#define DEFAULT_PORT 1883U
#define QUEUE_LEN 16
#define RES_QUEUE_LEN 8
#define TASK_STACK 4096
#define TASK_PRIO 2
#define TASK_CORE 0
#define CLIENT_STACK_TLS 8192 /* esp-mqtt task with a TLS handshake in it */
#define OUTBOX_LIMIT (32 * 1024)
#define JSON_MAX 1024
typedef enum {
    ST_OFF,
    ST_NO_HOST,
    ST_CONNECTING,
    ST_CONNECTED,
    ST_REFUSED,
    ST_TLS_ERROR, /* the TLS handshake failed: certificate not trusted, wrong name, no TLS there */
} state_t;

static QueueHandle_t s_q;
static QueueHandle_t s_rq; /* downlink results */
static QueueSetHandle_t s_qs;
static esp_mqtt_client_handle_t s_client;
static mqttc_cfg_t s_cfg;
static char s_pass[MQTTC_PASS_MAX];
static char s_status_topic[LWC_MQTT_TOPIC_MAX];
static char s_down_sub[LWC_MQTT_TOPIC_MAX]; /* lora/<gw_id>/+/down */
static atomic_uint s_down_ok, s_down_bad, s_results, s_res_dropped;
static atomic_int s_state = ST_OFF;
static atomic_bool s_connected_ev;
static atomic_uint s_published, s_dropped, s_failed, s_connects;
static int s_last_err; /* last failure logged (esp-mqtt task only) */

/* ---- settings --------------------------------------------------------------- */

static bool host_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0U || n >= MQTTC_HOST_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Printable ASCII; `space` allows ' ' (passwords). */
static bool text_ok(const char *s, size_t max, bool space)
{
    const size_t n = strlen(s);
    if (n >= max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < (space ? ' ' : '!') || s[i] > '~') {
            return false;
        }
    }
    return true;
}

static void default_gw_id(char out[MQTTC_GWID_MAX])
{
    char eui[17] = "";
    (void)gwmode_eui(eui);
    for (size_t i = 0; i < 16U; i++) {
        out[i] = (eui[i] >= 'A' && eui[i] <= 'F') ? (char)(eui[i] - 'A' + 'a') : eui[i];
    }
    out[16] = '\0';
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static void load(mqttc_cfg_t *c, char *pass, size_t pcap)
{
    memset(c, 0, sizeof *c);
    c->port = DEFAULT_PORT;
    if (pass != NULL) {
        pass[0] = '\0';
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        (void)nvs_get_u8(h, KEY_ON, &on);
        c->on = on != 0U;
        get_str(h, KEY_HOST, c->host, sizeof c->host);
        (void)nvs_get_u16(h, KEY_PORT, &c->port);
        get_str(h, KEY_USER, c->user, sizeof c->user);
        size_t n = 0;
        c->pass_set = nvs_get_str(h, KEY_PASS, NULL, &n) == ESP_OK && n > 1U;
        if (pass != NULL && c->pass_set) {
            get_str(h, KEY_PASS, pass, pcap);
        }
        get_str(h, KEY_GWID, c->gw_id, sizeof c->gw_id);
        uint8_t tls = 0;
        (void)nvs_get_u8(h, KEY_TLS, &tls);
        c->tls = tls != 0U;
        nvs_close(h);
    }
    if (!lwc_mqtt_id_ok(c->gw_id)) {
        default_gw_id(c->gw_id);
    }
    if (c->port == 0U) {
        c->port = DEFAULT_PORT;
    }
}

void mqttc_get(mqttc_cfg_t *c)
{
    load(c, NULL, 0);
}

static esp_err_t set_or_erase(nvs_handle_t h, const char *key, const char *v)
{
    if (v[0] != '\0') {
        return nvs_set_str(h, key, v);
    }
    const esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t mqttc_set(const bool *on, const char *host, uint16_t port, const char *user, const char *pass,
                    const char *gw_id, const bool *tls)
{
    ESP_RETURN_ON_FALSE(host == NULL || host[0] == '\0' || host_ok(host), ESP_ERR_INVALID_ARG, TAG, "host");
    ESP_RETURN_ON_FALSE(user == NULL || text_ok(user, MQTTC_USER_MAX, false), ESP_ERR_INVALID_ARG, TAG, "user");
    ESP_RETURN_ON_FALSE(pass == NULL || text_ok(pass, MQTTC_PASS_MAX, true), ESP_ERR_INVALID_ARG, TAG, "password");
    ESP_RETURN_ON_FALSE(gw_id == NULL || gw_id[0] == '\0' || lwc_mqtt_id_ok(gw_id), ESP_ERR_INVALID_ARG, TAG, "gw_id");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = ESP_OK;
    if (on != NULL) {
        err = nvs_set_u8(h, KEY_ON, *on ? 1U : 0U);
    }
    if (err == ESP_OK && host != NULL) {
        err = set_or_erase(h, KEY_HOST, host);
    }
    if (err == ESP_OK && port != 0U) {
        err = nvs_set_u16(h, KEY_PORT, port);
    }
    if (err == ESP_OK && user != NULL) {
        err = set_or_erase(h, KEY_USER, user);
    }
    if (err == ESP_OK && pass != NULL) {
        err = set_or_erase(h, KEY_PASS, pass);
    }
    if (err == ESP_OK && gw_id != NULL) {
        err = set_or_erase(h, KEY_GWID, gw_id);
    }
    if (err == ESP_OK && tls != NULL) {
        err = nvs_set_u8(h, KEY_TLS, *tls ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- publishing ------------------------------------------------------------- */

static bool publish(const char *topic, const char *data, size_t len, int qos, bool retain)
{
    if (atomic_load(&s_state) != ST_CONNECTED) {
        atomic_fetch_add(&s_dropped, 1U);
        return false;
    }
    const int id = esp_mqtt_client_publish(s_client, topic, data, (int)len, qos, retain ? 1 : 0);
    if (id < 0) {
        atomic_fetch_add(&s_failed, 1U);
        return false;
    }
    atomic_fetch_add(&s_published, 1U);
    return true;
}

static void handle(const mqttc_ev_t *ev)
{
    static char topic[LWC_MQTT_TOPIC_MAX], json[JSON_MAX];
    char node[17];
    lwc_mqtt_node_id(ev->dev_eui, node);
    if (atomic_load(&s_state) != ST_CONNECTED) {
        atomic_fetch_add(&s_dropped, 1U);
        return;
    }
    const lwc_mqtt_up_t u = {.node = node,
                           .name = ev->name,
                           .dev_eui = ev->dev_eui,
                           .dev_addr = ev->dev_addr,
                           .fcnt = ev->fcnt,
                           .port = ev->port,
                           .confirmed = ev->confirmed,
                           .data = ev->data,
                           .len = ev->len,
                           .rssi = ev->rssi,
                           .snr = ev->snr,
                           .sf = ev->sf,
                           .bw_khz = ev->bw_khz,
                           .freq_hz = ev->freq_hz,
                           .time_unix_s = ev->time_unix_s};
    const bool up = ev->kind == MQTTC_UP;
    const size_t n = up ? lwc_mqtt_up_json(&u, json, sizeof json) : lwc_mqtt_join_json(&u, json, sizeof json);
    if (n == 0U || !lwc_mqtt_topic(topic, sizeof topic, s_cfg.gw_id, node, up ? "up" : "join")) {
        atomic_fetch_add(&s_failed, 1U);
        return;
    }
    (void)publish(topic, json, n, 0, false);
}

static void handle_result(const mqttc_res_t *r)
{
    static char topic[LWC_MQTT_TOPIC_MAX], json[384];
    char node[17];
    lwc_mqtt_node_id(r->dev_eui, node);
    struct timeval tv;
    const bool clock = gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1700000000; /* set by SNTP */
    const lwc_mqtt_result_t m = {.node = node,
                                 .dev_eui = r->dev_eui,
                                 .id = r->id,
                                 .state = (lwc_mqtt_dl_state_t)r->state,
                                 .reason = r->reason,
                                 .port = r->port,
                                 .len = r->len,
                                 .confirmed = r->confirmed,
                                 .has_fcnt = r->has_fcnt,
                                 .fcnt_down = r->fcnt_down,
                                 .tx = r->tx,
                                 .time_unix_s = clock ? (int64_t)tv.tv_sec : -1};
    const size_t n = lwc_mqtt_result_json(&m, json, sizeof json);
    if (n == 0U || !lwc_mqtt_topic(topic, sizeof topic, s_cfg.gw_id, node, "down/result")) {
        atomic_fetch_add(&s_failed, 1U);
        return;
    }
    if (publish(topic, json, n, 1, false)) {
        atomic_fetch_add(&s_results, 1U);
    }
}

static void mqtt_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("mqtt", 30000);
    static mqttc_ev_t ev;
    static mqttc_res_t res;
    for (;;) {
        sv_beat(sv);
        if (atomic_exchange(&s_connected_ev, false)) {
            (void)publish(s_status_topic, "online", 6, 1, true);
            /* clean session: subscribe again after every connect */
            if (esp_mqtt_client_subscribe(s_client, s_down_sub, 1) < 0) {
                ESP_LOGW(TAG, "downlinks: subscribe failed");
            }
        }
        const QueueSetMemberHandle_t m = xQueueSelectFromSet(s_qs, pdMS_TO_TICKS(1000));
        if (m == s_q && xQueueReceive(s_q, &ev, 0) == pdTRUE) {
            handle(&ev);
        } else if (m == s_rq && xQueueReceive(s_rq, &res, 0) == pdTRUE) {
            handle_result(&res);
        }
    }
}

/* A downlink message (ADR-0018): never a retained one (it would be sent again
 * after every reconnect), only whole messages; an empty one (clearing a
 * retained message) is ignored. Runs in the esp-mqtt task. */
static void down_in(const esp_mqtt_event_t *e)
{
    uint64_t eui;
    if (e == NULL || e->retain || e->current_data_offset != 0 || e->data_len <= 0 ||
        e->data_len != e->total_data_len || !lwc_mqtt_down_topic(e->topic, (size_t)e->topic_len, s_cfg.gw_id, &eui)) {
        return;
    }
    static lwc_mqtt_down_t d; /* esp-mqtt task only */
    char node[17];
    lwc_mqtt_node_id(eui, node);
    const char *why = lwc_mqtt_down_parse(e->data, (size_t)e->data_len, &d);
    const bool parsed = why == NULL;
    if (why == NULL) {
        const esp_err_t err = lns_queue_down(eui, d.port, d.data, d.len, d.confirmed, d.id);
        why = err == ESP_OK                  ? NULL
              : err == ESP_ERR_NOT_FOUND     ? "no such local device"
              : err == ESP_ERR_NOT_SUPPORTED ? "local transmit is off"
              : err == ESP_ERR_INVALID_STATE ? "the local network server is not running"
              : err == ESP_ERR_INVALID_SIZE  ? "too big for the device's RX2 window"
              : err == ESP_ERR_NO_MEM        ? "the device's queue is full (4)"
                                             : "busy";
    }
    mqttc_res_t r = {.dev_eui = eui,
                     .state = why != NULL ? LWC_MQTT_DL_REFUSED : LWC_MQTT_DL_QUEUED,
                     .reason = why, /* a string literal (core or above) */
                     .port = parsed ? (int16_t)d.port : -1,
                     .len = parsed ? (int16_t)d.len : -1,
                     .confirmed = d.confirmed};
    memcpy(r.id, d.id, sizeof r.id);
    if (why != NULL) {
        atomic_fetch_add(&s_down_bad, 1U);
        /* at most one line a second: a flood of bad messages must not push
         * everything else out of the log ring */
        static int64_t last_us = -1000000;
        static unsigned quiet;
        const int64_t now = esp_timer_get_time();
        if (now - last_us >= 1000000) {
            ESP_LOGW(TAG, "downlink for %s refused: %s (%u more refused since the last line)", node, why, quiet);
            last_us = now;
            quiet = 0;
        } else {
            quiet++;
        }
    } else {
        atomic_fetch_add(&s_down_ok, 1U);
        ESP_LOGI(TAG, "downlink for %s queued: port %u, %u B%s", node, d.port, (unsigned)d.len,
                 d.confirmed ? ", confirmed" : "");
    }
    mqttc_result(&r);
    memset(&d, 0, sizeof d);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_last_err = 0;
        atomic_store(&s_state, ST_CONNECTED);
        atomic_fetch_add(&s_connects, 1U);
        atomic_store(&s_connected_ev, true);
        ESP_LOGI(TAG, "connected to %s:%u", s_cfg.host, (unsigned)s_cfg.port);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (atomic_load(&s_state) == ST_CONNECTED) {
            atomic_store(&s_state, ST_CONNECTING);
            ESP_LOGW(TAG, "disconnected: retrying every 10 s");
        }
        break;
    case MQTT_EVENT_ERROR:
        /* Logged once per kind of failure, not on every retry (the log ring
         * would fill with them while a broker is away). */
        if (e != NULL && e->error_handle != NULL) {
            const esp_mqtt_error_codes_t *x = e->error_handle;
            /* a TLS failure: the handshake itself, not the network */
            const bool tls_bad = s_cfg.tls && x->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT &&
                                 (x->esp_tls_cert_verify_flags != 0 || x->esp_tls_stack_err != 0);
            /* 0x2700: MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (the bundle path leaves the
             * flags 0; esp-tls reports the code without its sign) */
            const bool cert_bad = tls_bad && (x->esp_tls_cert_verify_flags != 0 || x->esp_tls_stack_err == 0x2700 ||
                                              x->esp_tls_stack_err == -0x2700);
            const int kind = x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED ? 1000 + (int)x->connect_return_code
                             : cert_bad ? 2000
                             : tls_bad  ? 3000 + (x->esp_tls_stack_err & 0xffff)
                                        : (int)x->error_type;
            if (x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                atomic_store(&s_state, ST_REFUSED);
            } else if (tls_bad) {
                atomic_store(&s_state, ST_TLS_ERROR);
            }
            if (kind != s_last_err) {
                s_last_err = kind;
                if (x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    ESP_LOGW(TAG, "broker refused the connection (code %d): check the user and password",
                             (int)x->connect_return_code);
                } else if (cert_bad) {
                    ESP_LOGW(TAG, "broker certificate not accepted (flags 0x%x): it must come from a public CA "
                                  "(e.g. Let's Encrypt), be valid now, and name the host set here",
                             (unsigned)x->esp_tls_cert_verify_flags);
                } else if (tls_bad) {
                    ESP_LOGW(TAG, "TLS handshake with the broker failed (mbedTLS -0x%04x): is TLS on at this port?",
                             (unsigned)(x->esp_tls_stack_err < 0 ? -x->esp_tls_stack_err : x->esp_tls_stack_err));
                } else {
                    ESP_LOGW(TAG, "cannot reach the broker (error type %d, socket errno %d): retrying every 10 s",
                             (int)x->error_type, x->esp_transport_sock_errno);
                }
            }
        }
        break;
    case MQTT_EVENT_DATA:
        down_in(e);
        break;
    case MQTT_EVENT_BEFORE_CONNECT:
        if (atomic_load(&s_state) != ST_REFUSED && atomic_load(&s_state) != ST_TLS_ERROR) {
            atomic_store(&s_state, ST_CONNECTING);
        }
        break;
    default:
        break;
    }
}

esp_err_t mqttc_start(void)
{
    load(&s_cfg, s_pass, sizeof s_pass);
    if (!s_cfg.on) {
        atomic_store(&s_state, ST_OFF);
        return ESP_OK;
    }
    if (s_cfg.host[0] == '\0') {
        atomic_store(&s_state, ST_NO_HOST);
        ESP_LOGW(TAG, "on, but no broker host set");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(lwc_mqtt_topic(s_status_topic, sizeof s_status_topic, s_cfg.gw_id, NULL, "status"),
                        ESP_ERR_INVALID_ARG, TAG, "gateway id");
    (void)snprintf(s_down_sub, sizeof s_down_sub, "lora/%s/+/down", s_cfg.gw_id); /* gw_id checked above */
    s_q = xQueueCreateWithCaps(QUEUE_LEN, sizeof(mqttc_ev_t), MALLOC_CAP_SPIRAM);
    s_rq = xQueueCreateWithCaps(RES_QUEUE_LEN, sizeof(mqttc_res_t), MALLOC_CAP_SPIRAM);
    s_qs = xQueueCreateSet(QUEUE_LEN + RES_QUEUE_LEN);
    ESP_RETURN_ON_FALSE(s_q != NULL && s_rq != NULL && s_qs != NULL, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xQueueAddToSet(s_q, s_qs) == pdPASS && xQueueAddToSet(s_rq, s_qs) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "queue set");
    static char client_id[24];
    char eui[17] = "";
    (void)gwmode_eui(eui);
    (void)snprintf(client_id, sizeof client_id, "lora-gw-%c%c%c%c%c%c", eui[10], eui[11], eui[12], eui[13], eui[14],
                   eui[15]);
    for (char *p = client_id; *p != '\0'; p++) {
        if (*p >= 'A' && *p <= 'F') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    const esp_mqtt_client_config_t cfg = {
        .broker = {.address = {.hostname = s_cfg.host,
                               .port = s_cfg.port,
                               .transport = s_cfg.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP},
                   .verification = {.crt_bundle_attach = s_cfg.tls ? esp_crt_bundle_attach : NULL}},
        .task = {.stack_size = s_cfg.tls ? CLIENT_STACK_TLS : 0}, /* 0: the Kconfig default */
        .credentials = {.username = s_cfg.user[0] != '\0' ? s_cfg.user : NULL,
                        .client_id = client_id,
                        .authentication.password = s_pass[0] != '\0' ? s_pass : NULL},
        .session = {.last_will = {.topic = s_status_topic, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1},
                    .keepalive = 60},
        .network = {.reconnect_timeout_ms = 10000, .timeout_ms = 10000},
        .buffer = {.size = 1536, .out_size = 1536},
        .outbox = {.limit = OUTBOX_LIMIT},
    };
    /* esp-mqtt and the transport layers log every failed retry (three lines
     * every 10 s for a TLS failure); this file logs each new failure once, and
     * the Basics Station link logs its own: silenced (the same levels as
     * lbsfwd.c quiet_transport_logs, which may run later). */
    static const char *const quiet[] = {"mqtt_client", "esp-tls", "esp-tls-mbedtls", "transport_base", "transport"};
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) {
        esp_log_level_set(quiet[i], ESP_LOG_NONE);
    }
    s_client = esp_mqtt_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_NO_MEM, TAG, "client");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    atomic_store(&s_state, ST_CONNECTING);
    const BaseType_t ok = xTaskCreatePinnedToCore(mqtt_task, "mqttc", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "start");
    ESP_LOGI(TAG, "broker %s:%u%s, gateway id %s, client id %s", s_cfg.host, (unsigned)s_cfg.port,
             s_cfg.tls ? " (TLS)" : "", s_cfg.gw_id, client_id);
    return ESP_OK;
}

void mqttc_post(const mqttc_ev_t *ev)
{
    if (s_q == NULL || ev == NULL) {
        return;
    }
    if (atomic_load(&s_state) != ST_CONNECTED || xQueueSend(s_q, ev, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1U);
    }
}

void mqttc_result(const mqttc_res_t *r)
{
    if (s_rq == NULL || r == NULL) {
        return;
    }
    /* refusals come from anyone who can publish: they may fill only half the
     * queue, so results of real downlinks (sent, acked, ...) keep room */
    const bool flood = r->state == LWC_MQTT_DL_REFUSED && uxQueueMessagesWaiting(s_rq) >= RES_QUEUE_LEN / 2U;
    if (flood || atomic_load(&s_state) != ST_CONNECTED || xQueueSend(s_rq, r, 0) != pdTRUE) {
        atomic_fetch_add(&s_res_dropped, 1U);
    }
}

void mqttc_status_line(char *buf, size_t len)
{
    static const char *const NAMES[] = {"off", "no_host", "connecting", "connected", "refused", "tls_error"};
    mqttc_cfg_t c;
    if (s_client != NULL || atomic_load(&s_state) == ST_NO_HOST) {
        c = s_cfg;
    } else {
        mqttc_get(&c); /* not started this boot: show what is stored */
    }
    const int st = atomic_load(&s_state);
    snprintf(buf, len,
             "state=%s tls=%s on=%s host=%s port=%u user=%s pass=%s gw_id=%s"
             " published=%u dropped=%u failed=%u connects=%u down_queued=%u down_refused=%u results=%u"
             " results_dropped=%u",
             NAMES[st >= 0 && st <= ST_TLS_ERROR ? st : 0], c.tls ? "on" : "off", c.on ? "on" : "off", c.host[0] ? c.host : "-",
             (unsigned)c.port, c.user[0] ? c.user : "-", c.pass_set ? "set" : "none", c.gw_id,
             atomic_load(&s_published), atomic_load(&s_dropped), atomic_load(&s_failed), atomic_load(&s_connects),
             atomic_load(&s_down_ok), atomic_load(&s_down_bad), atomic_load(&s_results),
             atomic_load(&s_res_dropped));
}
