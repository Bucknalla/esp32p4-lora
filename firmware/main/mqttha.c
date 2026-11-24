/*
 * MQTT to Home Assistant (ADR-0018).
 *
 * The local network server posts events with mqttha_post(), which never
 * blocks: a zero-timeout send to a queue in PSRAM. This task turns them into
 * JSON (core/src/ha.c) and publishes them. Uplinks and joins go out at QoS 0
 * and only while connected: when the broker is away they are dropped and
 * counted, so memory stays bounded. Discovery configs and the status are
 * retained QoS 1; the client's outbox is capped (OUTBOX_LIMIT, in PSRAM).
 *
 * On every connect: lora/<gw>/status "online" (the last will is "offline"),
 * the gateway's discovery config, and the configs of every local device.
 * A device added later is announced with its first join or uplink. A removed
 * device's configs are cleared with empty retained messages (only while
 * connected).
 *
 * The password is kept in NVS and given only to the client; it is never
 * logged or returned by the API.
 */
#include "mqttha.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gwmode.h"
#include "lns.h"
#include "lwc/ha.h"
#include "lwc/hex.h"
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
#define KEY_EXPIRE "expire"

#define DEFAULT_PORT 1883U
#define DEFAULT_EXPIRE_S 3600U
#define QUEUE_LEN 16
#define TASK_STACK 4096
#define TASK_PRIO 2
#define TASK_CORE 0
#define OUTBOX_LIMIT (32 * 1024)
#define JSON_MAX 1024
/* After each connect, retained sensor configs are read for SWEEP_MS. One that
 * points to this gateway (via_device) but names a device that is gone is
 * cleared: a device removed while the broker was away, MQTT was off or before
 * a restart. */
#define SWEEP_TOPIC "homeassistant/sensor/+/config"
#define SWEEP_PREFIX "homeassistant/sensor/"
#define SWEEP_MS 10000
#define KIND_STALE 100 /* internal event: a retained config to check */

typedef enum {
    ST_OFF,
    ST_NO_HOST,
    ST_CONNECTING,
    ST_CONNECTED,
    ST_REFUSED,
} state_t;

static QueueHandle_t s_q;
static esp_mqtt_client_handle_t s_client;
static mqttha_cfg_t s_cfg;
static char s_pass[MQTTHA_PASS_MAX];
static char s_status_topic[LWC_HA_TOPIC_MAX];
static char s_via[64]; /* "via_device":"lora_gw_<gw_id>" */
static atomic_int s_state = ST_OFF;
static atomic_bool s_announce;
static atomic_uint s_published, s_dropped, s_failed, s_connects;
/* Devices whose discovery configs went out on this connection. */
static uint64_t s_announced[LWC_NS_MAX_DEVICES];
static size_t s_n_announced;
static int s_last_err; /* last failure logged (esp-mqtt task only) */

/* ---- settings --------------------------------------------------------------- */

static bool host_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0U || n >= MQTTHA_HOST_MAX) {
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

static void default_gw_id(char out[MQTTHA_GWID_MAX])
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

static void load(mqttha_cfg_t *c, char *pass, size_t pcap)
{
    memset(c, 0, sizeof *c);
    c->port = DEFAULT_PORT;
    c->expire_s = DEFAULT_EXPIRE_S;
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
        (void)nvs_get_u32(h, KEY_EXPIRE, &c->expire_s);
        nvs_close(h);
    }
    if (!lwc_ha_id_ok(c->gw_id)) {
        default_gw_id(c->gw_id);
    }
    if (c->port == 0U) {
        c->port = DEFAULT_PORT;
    }
}

void mqttha_get(mqttha_cfg_t *c)
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

esp_err_t mqttha_set(const bool *on, const char *host, uint16_t port, const char *user, const char *pass,
                     const char *gw_id, int32_t expire_s)
{
    ESP_RETURN_ON_FALSE(host == NULL || host[0] == '\0' || host_ok(host), ESP_ERR_INVALID_ARG, TAG, "host");
    ESP_RETURN_ON_FALSE(user == NULL || text_ok(user, MQTTHA_USER_MAX, false), ESP_ERR_INVALID_ARG, TAG, "user");
    ESP_RETURN_ON_FALSE(pass == NULL || text_ok(pass, MQTTHA_PASS_MAX, true), ESP_ERR_INVALID_ARG, TAG, "password");
    ESP_RETURN_ON_FALSE(gw_id == NULL || gw_id[0] == '\0' || lwc_ha_id_ok(gw_id), ESP_ERR_INVALID_ARG, TAG, "gw_id");
    ESP_RETURN_ON_FALSE(expire_s < 0 || (uint32_t)expire_s <= MQTTHA_EXPIRE_MAX, ESP_ERR_INVALID_ARG, TAG, "expire");
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
    if (err == ESP_OK && expire_s >= 0) {
        err = nvs_set_u32(h, KEY_EXPIRE, (uint32_t)expire_s);
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

static bool announced(uint64_t eui)
{
    for (size_t i = 0; i < s_n_announced; i++) {
        if (s_announced[i] == eui) {
            return true;
        }
    }
    return false;
}

static void forget(uint64_t eui)
{
    for (size_t i = 0; i < s_n_announced; i++) {
        if (s_announced[i] == eui) {
            s_announced[i] = s_announced[--s_n_announced];
            return;
        }
    }
}

/* Discovery configs of one device; remembered only if all went out (a full
 * outbox: tried again with its next uplink). */
static void announce_dev(uint64_t eui, const char *name)
{
    static char topic[LWC_HA_TOPIC_MAX], json[JSON_MAX];
    char node[17];
    lwc_ha_node_id(eui, node);
    bool ok = true;
    for (int f = 0; f < (int)LWC_HA_FIELDS; f++) {
        const size_t n = lwc_ha_sensor_config(s_cfg.gw_id, node, name, (lwc_ha_field_t)f, s_cfg.expire_s, topic,
                                              sizeof topic, json, sizeof json);
        ok = n > 0U && publish(topic, json, n, 1, true) && ok;
    }
    if (ok && !announced(eui) && s_n_announced < LWC_NS_MAX_DEVICES) {
        s_announced[s_n_announced++] = eui;
    }
}

static void announce_all(void)
{
    static char topic[LWC_HA_TOPIC_MAX], json[JSON_MAX];
    s_n_announced = 0;
    (void)publish(s_status_topic, "online", 6, 1, true);
    const size_t n = lwc_ha_gateway_config(s_cfg.gw_id, topic, sizeof topic, json, sizeof json);
    if (n > 0U) {
        (void)publish(topic, json, n, 1, true);
    }
    static uint64_t euis[LWC_NS_MAX_DEVICES];
    static char names[LWC_NS_MAX_DEVICES][LWC_NS_NAME_MAX + 1U];
    const size_t nd = lns_device_names(euis, names, LWC_NS_MAX_DEVICES);
    for (size_t i = 0; i < nd; i++) {
        announce_dev(euis[i], names[i]);
        vTaskDelay(pdMS_TO_TICKS(20)); /* let PUBACKs drain the outbox */
    }
    ESP_LOGI(TAG, "announced the gateway and %u of %u device(s)", (unsigned)s_n_announced, (unsigned)nd);
}

static bool present(uint64_t eui)
{
    static uint64_t euis[LWC_NS_MAX_DEVICES];
    static char names[LWC_NS_MAX_DEVICES][LWC_NS_NAME_MAX + 1U];
    const size_t nd = lns_device_names(euis, names, LWC_NS_MAX_DEVICES);
    for (size_t i = 0; i < nd; i++) {
        if (euis[i] == eui) {
            return true;
        }
    }
    return false;
}

static void handle(const mqttha_ev_t *ev)
{
    static char topic[LWC_HA_TOPIC_MAX], json[JSON_MAX];
    char node[17];
    lwc_ha_node_id(ev->dev_eui, node);
    if (atomic_load(&s_state) != ST_CONNECTED) {
        atomic_fetch_add(&s_dropped, 1U);
        return;
    }
    if (ev->kind == KIND_STALE && present(ev->dev_eui)) {
        return;
    }
    if (ev->kind == MQTTHA_REMOVED || ev->kind == KIND_STALE) {
        forget(ev->dev_eui);
        for (int f = 0; f < (int)LWC_HA_FIELDS; f++) {
            if (lwc_ha_sensor_config(s_cfg.gw_id, node, NULL, (lwc_ha_field_t)f, 0, topic, sizeof topic, NULL, 0) > 0U) {
                (void)publish(topic, "", 0, 1, true);
            }
        }
        return;
    }
    if (!announced(ev->dev_eui)) {
        announce_dev(ev->dev_eui, ev->name);
    }
    const lwc_ha_up_t u = {.node = node,
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
    const bool up = ev->kind == MQTTHA_UP;
    const size_t n = up ? lwc_ha_up_json(&u, json, sizeof json) : lwc_ha_join_json(&u, json, sizeof json);
    if (n == 0U || !lwc_ha_topic(topic, sizeof topic, s_cfg.gw_id, node, up ? "up" : "join")) {
        atomic_fetch_add(&s_failed, 1U);
        return;
    }
    (void)publish(topic, json, n, 0, false);
}

static void mqtt_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("mqtt", 30000);
    static mqttha_ev_t ev;
    bool sweeping = false;
    TickType_t sweep_start = 0;
    for (;;) {
        sv_beat(sv);
        if (atomic_exchange(&s_announce, false)) {
            announce_all();
            sweeping = esp_mqtt_client_subscribe(s_client, SWEEP_TOPIC, 0) >= 0;
            sweep_start = xTaskGetTickCount();
            sv_beat(sv);
        }
        if (sweeping && xTaskGetTickCount() - sweep_start >= pdMS_TO_TICKS(SWEEP_MS)) {
            (void)esp_mqtt_client_unsubscribe(s_client, SWEEP_TOPIC);
            sweeping = false;
        }
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle(&ev);
        }
    }
}

/* A retained discovery config (sent on subscribe), whole, not yet cleared,
 * that points to this gateway: its DevEUI, to check in the mqttha task. */
static bool stale_candidate(const esp_mqtt_event_t *e, uint64_t *eui)
{
    const size_t pre = strlen(SWEEP_PREFIX);
    if (e == NULL || !e->retain || e->current_data_offset != 0 || e->data_len <= 0 ||
        e->data_len != e->total_data_len || e->topic_len <= (int)(pre + 17U + 7U) ||
        memcmp(e->topic, SWEEP_PREFIX, pre) != 0 || memcmp(e->topic + e->topic_len - 7, "/config", 7) != 0 ||
        e->topic[pre + 16U] != '_') {
        return false;
    }
    return memmem(e->data, (size_t)e->data_len, s_via, strlen(s_via)) != NULL &&
           lwc_hex_eui(e->topic + pre, 16, eui);
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
        atomic_store(&s_announce, true);
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
            const int kind = x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED ? 1000 + (int)x->connect_return_code
                                                                                  : (int)x->error_type;
            if (x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                atomic_store(&s_state, ST_REFUSED);
            }
            if (kind != s_last_err) {
                s_last_err = kind;
                if (x->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    ESP_LOGW(TAG, "broker refused the connection (code %d): check the user and password",
                             (int)x->connect_return_code);
                } else {
                    ESP_LOGW(TAG, "cannot reach the broker (error type %d, socket errno %d): retrying every 10 s",
                             (int)x->error_type, x->esp_transport_sock_errno);
                }
            }
        }
        break;
    case MQTT_EVENT_DATA: {
        mqttha_ev_t ev = {.kind = KIND_STALE};
        if (s_q != NULL && stale_candidate(e, &ev.dev_eui)) {
            (void)xQueueSend(s_q, &ev, 0);
        }
        break;
    }
    case MQTT_EVENT_BEFORE_CONNECT:
        if (atomic_load(&s_state) != ST_REFUSED) {
            atomic_store(&s_state, ST_CONNECTING);
        }
        break;
    default:
        break;
    }
}

esp_err_t mqttha_start(void)
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
    ESP_RETURN_ON_FALSE(lwc_ha_topic(s_status_topic, sizeof s_status_topic, s_cfg.gw_id, NULL, "status"),
                        ESP_ERR_INVALID_ARG, TAG, "gateway id");
    (void)snprintf(s_via, sizeof s_via, "\"via_device\":\"lora_gw_%s\"", s_cfg.gw_id);
    s_q = xQueueCreateWithCaps(QUEUE_LEN, sizeof(mqttha_ev_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_q != NULL, ESP_ERR_NO_MEM, TAG, "queue");
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
        .broker.address = {.hostname = s_cfg.host, .port = s_cfg.port, .transport = MQTT_TRANSPORT_OVER_TCP},
        .credentials = {.username = s_cfg.user[0] != '\0' ? s_cfg.user : NULL,
                        .client_id = client_id,
                        .authentication.password = s_pass[0] != '\0' ? s_pass : NULL},
        .session = {.last_will = {.topic = s_status_topic, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1},
                    .keepalive = 60},
        .network = {.reconnect_timeout_ms = 10000, .timeout_ms = 10000},
        .buffer = {.size = 1536, .out_size = 1536},
        .outbox = {.limit = OUTBOX_LIMIT},
    };
    /* esp-mqtt logs every failed retry; this file logs each new failure once. */
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    s_client = esp_mqtt_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_NO_MEM, TAG, "client");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    atomic_store(&s_state, ST_CONNECTING);
    const BaseType_t ok = xTaskCreatePinnedToCore(mqtt_task, "mqttha", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "start");
    ESP_LOGI(TAG, "broker %s:%u, gateway id %s, client id %s", s_cfg.host, (unsigned)s_cfg.port, s_cfg.gw_id,
             client_id);
    return ESP_OK;
}

void mqttha_post(const mqttha_ev_t *ev)
{
    if (s_q == NULL || ev == NULL) {
        return;
    }
    if (atomic_load(&s_state) != ST_CONNECTED || xQueueSend(s_q, ev, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1U);
    }
}

void mqttha_status_line(char *buf, size_t len)
{
    static const char *const NAMES[] = {"off", "no_host", "connecting", "connected", "refused"};
    mqttha_cfg_t c;
    if (s_client != NULL || atomic_load(&s_state) == ST_NO_HOST) {
        c = s_cfg;
    } else {
        mqttha_get(&c); /* not started this boot: show what is stored */
    }
    const int st = atomic_load(&s_state);
    snprintf(buf, len,
             "state=%s on=%s host=%s port=%u user=%s pass=%s gw_id=%s expire_s=%" PRIu32
             " published=%u dropped=%u failed=%u connects=%u",
             NAMES[st >= 0 && st <= ST_REFUSED ? st : 0], c.on ? "on" : "off", c.host[0] ? c.host : "-",
             (unsigned)c.port, c.user[0] ? c.user : "-", c.pass_set ? "set" : "none", c.gw_id, c.expire_s,
             atomic_load(&s_published), atomic_load(&s_dropped), atomic_load(&s_failed), atomic_load(&s_connects));
}
