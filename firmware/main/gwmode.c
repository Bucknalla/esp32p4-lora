#include "gwmode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "gwmode";

#define NVS_NS "pktfwd" /* historic name (the UDP forwarder): kept so stored settings survive */
#define KEY_MODE "mode"
#define KEY_TTN "ttn" /* before modes: 1 = report to TTN (default), 0 = off */

static int s_mode = -1;

lwc_mode_t gwmode(void)
{
    if (s_mode >= 0) {
        return (lwc_mode_t)s_mode;
    }
    uint8_t m = LWC_MODE_TTN;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, KEY_MODE, &m) != ESP_OK || m > LWC_MODE_HYBRID) {
            uint8_t ttn = 1;
            (void)nvs_get_u8(h, KEY_TTN, &ttn);
            /* "TTN off" had nothing local: the closest mode is standalone. */
            m = ttn != 0U ? LWC_MODE_TTN : LWC_MODE_STANDALONE;
        }
        nvs_close(h);
    }
    s_mode = m;
    return (lwc_mode_t)m;
}

bool gwmode_forwards(void)
{
    return gwmode() != LWC_MODE_STANDALONE;
}

bool gwmode_local(void)
{
    return gwmode() != LWC_MODE_TTN;
}

const char *gwmode_name(lwc_mode_t m)
{
    return m == LWC_MODE_TTN ? "ttn" : m == LWC_MODE_STANDALONE ? "standalone" : "hybrid";
}

bool gwmode_parse(const char *s, lwc_mode_t *out)
{
    for (int m = LWC_MODE_TTN; m <= LWC_MODE_HYBRID; m++) {
        if (strcmp(s, gwmode_name((lwc_mode_t)m)) == 0) {
            *out = (lwc_mode_t)m;
            return true;
        }
    }
    return false;
}

esp_err_t gwmode_set(lwc_mode_t m)
{
    ESP_RETURN_ON_FALSE(m <= LWC_MODE_HYBRID, ESP_ERR_INVALID_ARG, TAG, "mode");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(h, KEY_MODE, (uint8_t)m);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, KEY_TTN);
        err = (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- upstream link (Basics Station) ---------------------------------------------------------------- */

#define KEY_LBS_HOST "lbs_host"
#define KEY_LBS_KEY "lbs_key"

esp_err_t gwmode_eui(char out[17])
{
    uint8_t e[8];
    if (CONFIG_GW_EUI[0] != '\0') {
        ESP_RETURN_ON_FALSE(strlen(CONFIG_GW_EUI) == 16U, ESP_ERR_INVALID_ARG, TAG, "GW_EUI: 16 hex digits");
        for (size_t i = 0; i < 8U; i++) {
            const char b[3] = {CONFIG_GW_EUI[2U * i], CONFIG_GW_EUI[2U * i + 1U], '\0'};
            char *end = NULL;
            e[i] = (uint8_t)strtoul(b, &end, 16);
            ESP_RETURN_ON_FALSE(end == &b[2], ESP_ERR_INVALID_ARG, TAG, "GW_EUI: 16 hex digits");
        }
    } else {
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BASE), TAG, "base MAC");
        const uint8_t m[8] = {mac[0], mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5]};
        memcpy(e, m, sizeof e);
    }
    for (size_t i = 0; i < 8U; i++) {
        snprintf(&out[2U * i], 3, "%02X", e[i]);
    }
    return ESP_OK;
}

static esp_err_t get_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t n = cap;
    err = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    return err;
}

void gwmode_lbs_host(char out[GW_LBS_HOST_MAX])
{
    if (get_str(KEY_LBS_HOST, out, GW_LBS_HOST_MAX) != ESP_OK || out[0] == '\0') {
        strlcpy(out, CONFIG_GW_LBS_HOST, GW_LBS_HOST_MAX);
    }
}

bool gwmode_lbs_key_set(void)
{
    char k[GW_LBS_KEY_MAX];
    const bool set = get_str(KEY_LBS_KEY, k, sizeof k) == ESP_OK && k[0] != '\0';
    memset(k, 0, sizeof k);
    return set;
}

esp_err_t gwmode_lbs_key(char out[GW_LBS_KEY_MAX])
{
    return get_str(KEY_LBS_KEY, out, GW_LBS_KEY_MAX);
}

static bool host_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0U || n >= GW_LBS_HOST_MAX) {
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

static bool key_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n >= GW_LBS_KEY_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] <= ' ' || s[i] > '~' || s[i] == '"' || s[i] == '\\') {
            return false;
        }
    }
    return true;
}

esp_err_t gwmode_set_lbs(const char *host, const char *key)
{
    ESP_RETURN_ON_FALSE(host == NULL || host_ok(host), ESP_ERR_INVALID_ARG, TAG, "host");
    ESP_RETURN_ON_FALSE(key == NULL || key_ok(key), ESP_ERR_INVALID_ARG, TAG, "key");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = ESP_OK;
    if (host != NULL) {
        err = nvs_set_str(h, KEY_LBS_HOST, host);
    }
    if (err == ESP_OK && key != NULL) {
        if (key[0] == '\0') {
            err = nvs_erase_key(h, KEY_LBS_KEY);
            err = (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
        } else {
            err = nvs_set_str(h, KEY_LBS_KEY, key);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

