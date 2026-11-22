#include "wifi.h"

#include <stdatomic.h>
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
#define SCAN_MAX_APS 10

static atomic_bool s_has_ip;
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

static void log_scan(void)
{
    const wifi_scan_config_t scan = {0};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
        return;
    }
    uint16_t n = SCAN_MAX_APS;
    wifi_ap_record_t aps[SCAN_MAX_APS];
    err = esp_wifi_scan_get_ap_records(&n, aps);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan records: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "scan: %u network(s) (C6 link OK)", n);
    for (uint16_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  ch %2u  %4d dBm  %s", aps[i].primary, aps[i].rssi, (const char *)aps[i].ssid);
    }
}

esp_err_t wifi_start(void)
{
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG, "netif");

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init (C6 link)");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    wifi_config_t cfg = {0};
    const esp_err_t cred = load_credentials(&cfg);
    if (cred != ESP_OK) {
        ESP_LOGW(TAG, "no credentials in NVS (%s): scan only. Run tools/provision_wifi.py",
                 esp_err_to_name(cred));
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        log_scan();
        return ESP_OK;
    }

    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                                   NULL),
                        TAG, "register WIFI_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "register IP_EVENT");

    ESP_LOGI(TAG, "connecting to \"%s\"", (const char *)cfg.sta.ssid);
    memset(cfg.sta.password, 0, sizeof cfg.sta.password); /* do not keep a copy */
    return esp_wifi_start();
}
