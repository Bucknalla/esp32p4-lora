#include "wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwc/dns.h"
#include "esp_mac.h"
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
static esp_timer_handle_t s_setup_timer;
static atomic_bool s_setup;
static char s_nets[WIFI_SETUP_MAX_NETS][33];
static size_t s_net_count;
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

/* Scan once (proves the C6 link) and keep the names for the setup page. */
static void scan_networks(void)
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
    for (uint16_t i = 0; i < n && s_net_count < WIFI_SETUP_MAX_NETS; i++) {
        bool seen = aps[i].ssid[0] == 0; /* hidden network */
        for (size_t k = 0; k < s_net_count && !seen; k++) {
            seen = strncmp(s_nets[k], (const char *)aps[i].ssid, 32) == 0; /* one name, several APs */
        }
        if (!seen) { /* records come strongest first */
            memcpy(s_nets[s_net_count], aps[i].ssid, 32);
            s_nets[s_net_count++][32] = '\0';
        }
    }
}

static void setup_end_cb(void *arg)
{
    (void)arg;
    atomic_store(&s_setup, false);
    ESP_LOGW(TAG, "setup access point closed after %d min: power-cycle to open it again",
             WIFI_SETUP_MINUTES);
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "setup AP: a device joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "setup AP: a device left");
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "setup AP: gave " IPSTR " to a device", IP2STR(&e->ip));
    }
}

/* Captive-portal DNS (ADR-0013): every A query on the setup network is
 * answered with the gateway's address, so phones open their sign-in sheet. */
static void dns_task(void *arg)
{
    (void)arg;
    static const uint8_t ip[4] = {192, 168, 4, 1};
    static uint8_t q[512], r[600];
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(53)};
    const struct timeval tv = {.tv_sec = 1};
    if (sock < 0 || bind(sock, (struct sockaddr *)&me, sizeof me) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        ESP_LOGE(TAG, "captive DNS: socket");
    } else {
        while (atomic_load(&s_setup)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            const int n = recvfrom(sock, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                const size_t m = lwc_dns_captive_reply(q, (size_t)n, ip, r, sizeof r);
                if (m > 0) {
                    (void)sendto(sock, r, m, 0, (struct sockaddr *)&from, fl);
                }
            }
        }
    }
    if (sock >= 0) {
        close(sock);
    }
    vTaskDelete(NULL);
}

/* No credentials: an open access point for a few minutes, so a phone can
 * reach the setup page (ADR-0011, ADR-0013). */
static esp_err_t start_setup_ap(void)
{
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL, ESP_FAIL, TAG, "ap netif");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    wifi_config_t ap = {0};
    ap.ap.ssid_len = (uint8_t)snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid,
                                       "lorawan-gateway-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    (void)esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, NULL);
    (void)esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, NULL);
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_ap_event, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    scan_networks();
    const esp_timer_create_args_t targs = {.callback = setup_end_cb, .name = "wifi_setup"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_setup_timer), TAG, "setup timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_once(s_setup_timer, WIFI_SETUP_MINUTES * 60ULL * 1000000ULL),
                        TAG, "setup timer");
    atomic_store(&s_setup, true);
    if (xTaskCreatePinnedToCore(dns_task, "dns", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "captive DNS: task");
    }
    ESP_LOGW(TAG, "no Wi-Fi credentials: setup access point \"%s\" open for %d min; "
                  "join it and open https://192.168.4.1/",
             (const char *)ap.ap.ssid, WIFI_SETUP_MINUTES);
    return ESP_OK;
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

bool wifi_setup_active(void)
{
    return atomic_load(&s_setup);
}

size_t wifi_setup_networks(char names[][33], size_t max)
{
    const size_t n = s_net_count < max ? s_net_count : max;
    memcpy(names, s_nets, n * sizeof s_nets[0]);
    return n;
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
        ESP_LOGW(TAG, "no credentials in NVS (%s)", esp_err_to_name(cred));
        return start_setup_ap();
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
