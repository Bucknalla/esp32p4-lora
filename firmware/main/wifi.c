#include "wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define BACKOFF_MIN_MS 1000U
#define BACKOFF_MAX_MS 60000U

static atomic_bool s_has_ip;
static atomic_bool s_configured;
static _Atomic uint8_t s_last_reason; /* of the last failed attempt, 0 after an IP */
static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static uint32_t s_disconnects;

bool wifi_has_ip(void)
{
    return atomic_load(&s_has_ip);
}

static void schedule_retry(void);

static void retry_cb(void *arg)
{
    (void)arg;
    if (!atomic_load(&s_configured)) {
        return; /* idle station: nothing to join until provisioning */
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* No connect attempt, so no DISCONNECTED event will retry for us. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void schedule_retry(void)
{
    ESP_LOGI(TAG, "reconnect in %u ms", (unsigned)s_backoff_ms);
    (void)esp_timer_stop(s_retry_timer); /* a new network may be applied while one is pending */
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retry timer: %s", esp_err_to_name(err));
    }
    s_backoff_ms = (s_backoff_ms * 2U > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : s_backoff_ms * 2U;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        retry_cb(NULL);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "associated");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        atomic_store(&s_has_ip, false);
        if (!atomic_load(&s_configured)) {
            break; /* scans on an idle station end here too */
        }
        atomic_store(&s_last_reason, ev->reason);
        s_disconnects++;
        ESP_LOGW(TAG, "disconnected (reason %d, count %u)", ev->reason, (unsigned)s_disconnects);
        schedule_retry();
        break;
    }
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    atomic_store(&s_has_ip, true);
    atomic_store(&s_last_reason, 0);
    s_backoff_ms = BACKOFF_MIN_MS;
    ESP_LOGI(TAG, "IP " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
}

/* Reads SSID and password. The password is never logged. */
static esp_err_t load_credentials(wifi_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* NVS adds a NUL; a 32-byte SSID fills sta.ssid with none. */
    char ssid[sizeof cfg->sta.ssid + 1];
    size_t len = sizeof ssid;
    err = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        memcpy(cfg->sta.ssid, ssid, len - 1U);
        len = sizeof cfg->sta.password;
        err = nvs_get_str(h, WIFI_NVS_KEY_PASS, (char *)cfg->sta.password, &len);
    }
    nvs_close(h);
    return err;
}

esp_err_t wifi_store_credentials(const char *ssid, const char *pass)
{
    const size_t sl = strlen(ssid), pl = strlen(pass);
    ESP_RETURN_ON_FALSE(sl >= 1U && sl <= 32U && pl >= 8U && pl <= 63U, ESP_ERR_INVALID_ARG, TAG,
                        "length");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool is_auth_reason(uint8_t r)
{
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_EXPIRE ||
           r == WIFI_REASON_MIC_FAILURE || r == WIFI_REASON_CONNECTION_FAIL;
}

static bool is_not_found_reason(uint8_t r)
{
    return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

wifi_link_t wifi_link(void)
{
    if (atomic_load(&s_has_ip)) {
        return WIFI_LINK_UP;
    }
    if (!atomic_load(&s_configured)) {
        return WIFI_LINK_UNCONFIGURED;
    }
    const uint8_t r = atomic_load(&s_last_reason);
    if (is_auth_reason(r)) {
        return WIFI_LINK_AUTH_FAILED;
    }
    if (is_not_found_reason(r)) {
        return WIFI_LINK_NOT_FOUND;
    }
    return WIFI_LINK_CONNECTING;
}

bool wifi_configured(void)
{
    return atomic_load(&s_configured);
}

esp_err_t wifi_apply_credentials(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(wifi_store_credentials(ssid, pass), TAG, "store");
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    (void)esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof cfg); /* do not keep a copy */
    ESP_RETURN_ON_ERROR(err, TAG, "set config");
    atomic_store(&s_last_reason, 0);
    atomic_store(&s_configured, true);
    s_backoff_ms = BACKOFF_MIN_MS;
    ESP_LOGI(TAG, "new network \"%s\": connecting", ssid);
    schedule_retry();
    return ESP_OK;
}

esp_err_t wifi_scan(wifi_ap_record_t *aps, uint16_t *n)
{
    const wifi_scan_config_t scan = {0};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(n, aps);
    }
    if (err != ESP_OK) {
        *n = 0;
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_start(void)
{
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG, "netif");

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init (C6 link)");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                                   NULL),
                        TAG, "register WIFI_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "register IP_EVENT");

    wifi_config_t cfg = {0};
    const esp_err_t cred = load_credentials(&cfg);
    if (cred != ESP_OK) {
        /* Station started but idle: BLE provisioning (ble_prov.c) scans with
         * it and calls wifi_apply_credentials(). */
        ESP_LOGW(TAG, "no credentials in NVS (%s)", esp_err_to_name(cred));
        return esp_wifi_start();
    }

    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
    atomic_store(&s_configured, true);

    ESP_LOGI(TAG, "connecting to \"%s\"", (const char *)cfg.sta.ssid);
    memset(cfg.sta.password, 0, sizeof cfg.sta.password); /* do not keep a copy */
    return esp_wifi_start();
}
