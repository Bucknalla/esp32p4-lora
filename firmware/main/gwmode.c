#include "gwmode.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"

static const char *TAG = "gwmode";

#define NVS_NS "pktfwd"
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
