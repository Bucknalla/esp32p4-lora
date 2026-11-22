/*
 * BLE Wi-Fi provisioning (ADR-0014). The P4 runs NimBLE; the C6 is the BLE
 * controller (HCI over the ESP-Hosted SDIO link). The GATT service and the
 * protobuf endpoints are Espressif's provisioning protocol, so the stock
 * "ESP BLE Provisioning" app works: protocomm with security 1 and no PoP
 * (a short open window: physical presence), and our own handlers so that
 * wifi.c keeps the credentials (NVS "wifi") and the reconnect logic.
 *
 * Window: 15 min at boot without credentials, or after the HAT button.
 * After the phone sees "connected", BLE stays up DONE_GRACE_MS, then stops.
 */
#include "ble_prov.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_hosted_bt_host_stack.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/network_config.h"
#include "network_provisioning/network_scan.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security1.h"
#include "sv.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "ble_prov";

#define SCAN_MAX 16
#define DONE_GRACE_MS 30000
#define EV_OPEN 1U

/* Espressif provisioning service and characteristic UUIDs (as the stock
 * network_provisioning BLE scheme): the app looks for these. */
static const uint8_t SERVICE_UUID[16] = {0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87,
                                         0x9b, 0x43, 0x43, 0x6b, 0x4d, 0x24, 0x75, 0x17};
static protocomm_ble_name_uuid_t s_lookup[] = {
    {"prov-scan", 0xFF50},
    {"prov-session", 0xFF51},
    {"prov-config", 0xFF52},
    {"proto-ver", 0xFF53},
};
/* sec_ver 1, no PoP; scan supported. */
static const char VERSION_JSON[] =
    "{\"prov\":{\"ver\":\"netprov-v1.2\",\"sec_ver\":1,\"cap\":[\"no_pop\",\"wifi_prov\",\"wifi_scan\"]}}";

static char s_name[16];
static atomic_bool s_active;
static bool s_stack_up;
static protocomm_t *s_pc;
static TaskHandle_t s_task;
static const char *_Atomic s_why;

/* Touched only from the NimBLE host task (protocomm handlers). */
static wifi_ap_record_t s_aps[SCAN_MAX];
static uint16_t s_ap_count;
static char s_ssid[33];
static char s_pass[64];
static atomic_bool s_applied;

bool ble_prov_active(void)
{
    return atomic_load(&s_active);
}

const char *ble_prov_name(void)
{
    return s_name;
}

void ble_prov_open(const char *why)
{
    atomic_store(&s_why, why);
    if (s_task != NULL) {
        (void)xTaskNotify(s_task, EV_OPEN, eSetBits);
    }
}

/* ---- prov-scan ------------------------------------------------------------ */

static esp_err_t scan_start(bool blocking, bool passive, uint8_t group, uint32_t period_ms,
                            network_prov_scan_ctx_t **ctx)
{
    (void)blocking; /* always blocking: one short scan, the app polls after */
    (void)passive;
    (void)group;
    (void)period_ms;
    (void)ctx;
    s_ap_count = SCAN_MAX;
    if (wifi_scan(s_aps, &s_ap_count) != ESP_OK) {
        s_ap_count = 0;
    }
    ESP_LOGI(TAG, "scan for the app: %u network(s)", (unsigned)s_ap_count);
    return ESP_OK;
}

static esp_err_t scan_status(bool *finished, uint16_t *count, network_prov_scan_ctx_t **ctx)
{
    (void)ctx;
    *finished = true;
    *count = s_ap_count;
    return ESP_OK;
}

static esp_err_t scan_result(uint16_t i, network_prov_scan_wifi_result_t *r,
                             network_prov_scan_ctx_t **ctx)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(i < s_ap_count, ESP_ERR_INVALID_ARG, TAG, "scan index");
    memcpy(r->ssid, s_aps[i].ssid, sizeof r->ssid);
    memcpy(r->bssid, s_aps[i].bssid, sizeof r->bssid);
    r->channel = s_aps[i].primary;
    r->rssi = s_aps[i].rssi;
    r->auth = (uint8_t)s_aps[i].authmode;
    return ESP_OK;
}

static network_prov_scan_handlers_t s_scan_handlers = {
    .wifi_scan_start = scan_start,
    .wifi_scan_status = scan_status,
    .wifi_scan_result = scan_result,
};

/* ---- prov-config ---------------------------------------------------------- */

static esp_err_t get_status(network_prov_config_get_wifi_data_t *out, network_prov_ctx_t **ctx)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    switch (wifi_link()) {
    case WIFI_LINK_UP: {
        out->wifi_state = NETWORK_PROV_WIFI_STA_CONNECTED;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(out->conn_info.ssid, ap.ssid, sizeof out->conn_info.ssid - 1U);
            memcpy(out->conn_info.bssid, ap.bssid, sizeof out->conn_info.bssid);
            out->conn_info.channel = ap.primary;
            out->conn_info.auth_mode = (uint8_t)ap.authmode;
        }
        esp_netif_ip_info_t ip;
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif != NULL && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
            (void)esp_ip4addr_ntoa(&ip.ip, out->conn_info.ip_addr, sizeof out->conn_info.ip_addr);
        }
        break;
    }
    case WIFI_LINK_AUTH_FAILED:
        out->wifi_state = NETWORK_PROV_WIFI_STA_CONN_ATTEMPT_FAILED;
        out->fail_reason = NETWORK_PROV_WIFI_STA_AUTH_ERROR;
        break;
    case WIFI_LINK_NOT_FOUND:
        out->wifi_state = NETWORK_PROV_WIFI_STA_CONN_ATTEMPT_FAILED;
        out->fail_reason = NETWORK_PROV_WIFI_STA_AP_NOT_FOUND;
        break;
    case WIFI_LINK_UNCONFIGURED:
        out->wifi_state = NETWORK_PROV_WIFI_STA_DISCONNECTED;
        break;
    case WIFI_LINK_CONNECTING:
    default:
        out->wifi_state = NETWORK_PROV_WIFI_STA_CONNECTING;
        break;
    }
    return ESP_OK;
}

static esp_err_t set_config(const network_prov_config_set_wifi_data_t *req, network_prov_ctx_t **ctx)
{
    (void)ctx;
    const size_t sl = strnlen(req->ssid, sizeof req->ssid);
    const size_t pl = strnlen(req->password, sizeof req->password);
    /* WPA2 or newer only, as wifi_store_credentials(): no open networks. */
    if (sl < 1U || sl > 32U || pl < 8U || pl > 63U) {
        ESP_LOGW(TAG, "rejected network: SSID 1-32 bytes, password 8-63 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_ssid, req->ssid, sl);
    s_ssid[sl] = '\0';
    memcpy(s_pass, req->password, pl);
    s_pass[pl] = '\0';
    ESP_LOGI(TAG, "network \"%s\" received", s_ssid); /* never the password */
    return ESP_OK;
}

static esp_err_t apply_config(network_prov_ctx_t **ctx)
{
    (void)ctx;
    const esp_err_t err = wifi_apply_credentials(s_ssid, s_pass);
    memset(s_pass, 0, sizeof s_pass);
    if (err == ESP_OK) {
        atomic_store(&s_applied, true);
    }
    return err;
}

static network_prov_config_handlers_t s_cfg_handlers = {
    .wifi_get_status_handler = get_status,
    .wifi_set_config_handler = set_config,
    .wifi_apply_config_handler = apply_config,
};

/* ---- open / close --------------------------------------------------------- */

static void on_ble_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
        ESP_LOGI(TAG, "phone connected");
    } else if (id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
        ESP_LOGI(TAG, "phone disconnected");
    }
}

static esp_err_t open_window(void)
{
    if (!s_stack_up) {
        esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
        const esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no BLE controller on the C6 (%s): BLE setup unavailable. "
                          "Flash the C6 image with Bluetooth (tools/flash_c6_image.sh).",
                     esp_err_to_name(err));
            return err;
        }
        s_stack_up = true;
    }
    s_pc = protocomm_new();
    ESP_RETURN_ON_FALSE(s_pc != NULL, ESP_ERR_NO_MEM, TAG, "protocomm");
    protocomm_ble_config_t cfg = {
        .nu_lookup = s_lookup,
        .nu_lookup_count = sizeof s_lookup / sizeof s_lookup[0],
    };
    strlcpy(cfg.device_name, s_name, sizeof cfg.device_name);
    memcpy(cfg.service_uuid, SERVICE_UUID, sizeof cfg.service_uuid);
    esp_err_t err = protocomm_ble_start(s_pc, &cfg);
    if (err == ESP_OK) {
        err = protocomm_set_version(s_pc, "proto-ver", VERSION_JSON);
    }
    if (err == ESP_OK) {
        err = protocomm_set_security(s_pc, "prov-session", &protocomm_security1, NULL);
    }
    if (err == ESP_OK) {
        err = protocomm_add_endpoint(s_pc, "prov-config", network_prov_config_data_handler,
                                     &s_cfg_handlers);
    }
    if (err == ESP_OK) {
        err = protocomm_add_endpoint(s_pc, "prov-scan", network_prov_scan_handler, &s_scan_handlers);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        (void)protocomm_ble_stop(s_pc);
        protocomm_delete(s_pc);
        s_pc = NULL;
        return err;
    }
    atomic_store(&s_applied, false);
    atomic_store(&s_active, true);
    return ESP_OK;
}

static void close_window(const char *why)
{
    (void)protocomm_ble_stop(s_pc);
    protocomm_delete(s_pc);
    s_pc = NULL;
    atomic_store(&s_active, false);
    ESP_LOGI(TAG, "BLE setup closed (%s)", why);
}

static void prov_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("ble", 30000);
    int64_t close_at = 0;
    int64_t done_at = 0;
    for (;;) {
        sv_beat(sv);
        uint32_t ev = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(1000));
        sv_beat(sv);
        const int64_t now = esp_timer_get_time();
        if ((ev & EV_OPEN) != 0U) {
            const char *why = atomic_load(&s_why);
            if (atomic_load(&s_active) || open_window() == ESP_OK) {
                close_at = now + BLE_PROV_MINUTES * 60LL * 1000000LL;
                done_at = 0;
                ESP_LOGW(TAG, "BLE setup open for %d min (%s): use the ESP BLE Provisioning app, "
                              "device \"%s\"",
                         BLE_PROV_MINUTES, why != NULL ? why : "?", s_name);
            }
        }
        if (!atomic_load(&s_active)) {
            continue;
        }
        if (atomic_load(&s_applied) && wifi_has_ip() && done_at == 0) {
            done_at = now + DONE_GRACE_MS * 1000LL;
            web_claim_restart(); /* the new LAN owner sets the admin password */
        }
        if (done_at != 0 && now >= done_at) {
            close_window("Wi-Fi joined");
        } else if (now >= close_at) {
            close_window("time is up");
        }
    }
}

esp_err_t ble_prov_start(void)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    (void)snprintf(s_name, sizeof s_name, "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
    (void)esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, on_ble_event,
                                     NULL);
    /* NimBLE start, stop and the window timers run here, never in an
     * event handler or the NimBLE host task. */
    if (xTaskCreatePinnedToCore(prov_task, "ble_prov", 4096, NULL, 3, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!wifi_configured()) {
        ble_prov_open("no Wi-Fi credentials");
    }
    return ESP_OK;
}
