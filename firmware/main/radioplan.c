#include "radioplan.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwc/ns.h"
#include "lwc/txpk.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "radioplan";

#define NVS_NS "radio"
#define KEY_PLAN "plan"
#define KEY_SRC "src"

#ifndef CONFIG_GW_LNS_TX
#define CONFIG_GW_LNS_TX 0
#endif
#ifndef CONFIG_GW_TX_ENABLE
#define CONFIG_GW_TX_ENABLE 0
#endif
#define KEY_GAIN "gain"
#define KEY_TX "tx"
#define KEY_LNS_TX "lns_tx"
#define KEY_RX1 "rx1"
#define KEY_RX2_HZ "rx2hz"
#define KEY_RX2_DR "rx2dr"
#define KEY_REGION "region"
#define KEY_CARD "card"
#define EIRP_LIMIT_DB10 161 /* EU868 14 dBm ERP = 16.15 dBm EIRP (ADR-0007), rounded down */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_loaded;
static lwc_plan_t s_plan;
static radioplan_src_t s_src;
static lwc_plan_t s_prev; /* stored before s_plan (for a revert) */
static radioplan_src_t s_prev_src;
static lwc_plan_t s_bad;  /* the radio could not start with it */
static bool s_have_bad;
static radio_tx_cfg_t s_tx;
static radio_rx_cfg_t s_rx;
static const lwc_region_t *s_region;
static const lwc_card_t *s_card;

/* The region a card starts in when none (or one it cannot serve) is stored. */
static const lwc_region_t *card_default_region(const lwc_card_t *c)
{
    static const lwc_region_id_t def[LWC_CARDS] = {
        [LWC_CARD_868] = LWC_REGION_EU868,
        [LWC_CARD_915] = LWC_REGION_US915,
        [LWC_CARD_433] = LWC_REGION_EU433,
        [LWC_CARD_470] = LWC_REGION_CN470,
    };
    return lwc_region(def[c->id]);
}

static void lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf); /* first call: before other tasks use it */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Call with the lock held. */
static void load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_card = lwc_card(LWC_CARD_868);
    s_tx = (radio_tx_cfg_t){30, CONFIG_GW_TX_ENABLE != 0, CONFIG_GW_LNS_TX != 0};
    nvs_handle_t h;
    const bool have_nvs = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    uint8_t cid, rid;
    const bool have_region = have_nvs && nvs_get_u8(h, KEY_REGION, &rid) == ESP_OK;
    const lwc_region_t *stored = have_region ? lwc_region((lwc_region_id_t)rid) : NULL;
    const lwc_card_t *card = have_nvs && nvs_get_u8(h, KEY_CARD, &cid) == ESP_OK ? lwc_card((lwc_card_id_t)cid) : NULL;
    if (card == NULL && stored != NULL) {
        /* no card stored (set up before ADR-0025): the card of the stored region,
         * so a receive-only region stays receive-only */
        card = lwc_card_for_region(stored);
    }
    if (card != NULL) {
        s_card = card;
    }
    s_region = card_default_region(s_card);
    if (stored != NULL && lwc_card_region_ok(s_card, stored)) {
        s_region = stored;
    } else if (have_region) {
        ESP_LOGW(TAG, "stored region %u not usable with the %s MHz card: %s", (unsigned)rid, s_card->name,
                 s_region->name);
    }
    (void)lwc_plan_default(&s_plan, s_region);
    s_src = RADIOPLAN_DEFAULT;
    s_prev = s_plan;
    s_prev_src = s_src;
    s_rx = (radio_rx_cfg_t){1, s_region->rx2_hz, s_region->rx2_dr};
    if (!have_nvs) {
        return;
    }
    uint8_t b[LWC_PLAN_PACKED];
    size_t n = sizeof b;
    uint8_t src = RADIOPLAN_DEFAULT;
    lwc_plan_t p;
    if (nvs_get_blob(h, KEY_PLAN, b, &n) == ESP_OK) {
        if (lwc_plan_unpack(b, n, s_region, s_card, &p) && nvs_get_u8(h, KEY_SRC, &src) == ESP_OK &&
            src <= RADIOPLAN_SERVER) {
            s_plan = p; /* s_prev stays the default: a stored plan that fails falls back to it */
            s_src = (radioplan_src_t)src;
        } else {
            ESP_LOGW(TAG, "stored plan not valid: using the %s default", s_region->name);
        }
    }
    int16_t g;
    uint8_t v;
    if (nvs_get_i16(h, KEY_GAIN, &g) == ESP_OK && g >= RADIO_GAIN_MIN_DB10 && g <= RADIO_GAIN_MAX_DB10) {
        s_tx.gain_db10 = g;
    }
    if (nvs_get_u8(h, KEY_TX, &v) == ESP_OK) {
        s_tx.tx_on = v != 0U && CONFIG_GW_TX_ENABLE;
    }
    if (nvs_get_u8(h, KEY_LNS_TX, &v) == ESP_OK) {
        s_tx.lns_tx = v != 0U;
    }
    radio_rx_cfg_t rx = s_rx;
    uint32_t hz;
    if (nvs_get_u8(h, KEY_RX1, &rx.rx1_delay_s) == ESP_OK && nvs_get_u32(h, KEY_RX2_HZ, &hz) == ESP_OK &&
        nvs_get_u8(h, KEY_RX2_DR, &rx.rx2_dr) == ESP_OK) {
        rx.rx2_hz = hz;
        if (lwc_ns_rx_ok(s_region, rx.rx1_delay_s, rx.rx2_hz, rx.rx2_dr)) {
            s_rx = rx;
        }
    }
    nvs_close(h);
}

const lwc_region_t *radioplan_region(void)
{
    lock();
    load();
    const lwc_region_t *r = s_region;
    unlock();
    return r;
}

const lwc_card_t *radioplan_card(void)
{
    lock();
    load();
    const lwc_card_t *c = s_card;
    unlock();
    return c;
}

esp_err_t radioplan_set_region(const lwc_card_t *c, const lwc_region_t *r)
{
    ESP_RETURN_ON_FALSE(c != NULL && r != NULL, ESP_ERR_INVALID_ARG, TAG, "card, region");
    ESP_RETURN_ON_FALSE(lwc_card_region_ok(c, r), ESP_ERR_INVALID_ARG, TAG, "%s: not on the %s MHz card", r->name,
                        c->name);
    lock();
    load();
    const lwc_region_t *old = s_region;
    const lwc_card_t *old_card = s_card;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_CARD, (uint8_t)c->id);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_REGION, (uint8_t)r->id);
        }
        if (err == ESP_OK && (r != old || c != old_card)) { /* another region or card: back to the defaults */
            const char *keys[] = {KEY_PLAN, KEY_SRC, KEY_RX1, KEY_RX2_HZ, KEY_RX2_DR};
            for (size_t i = 0; err == ESP_OK && i < sizeof keys / sizeof keys[0]; i++) {
                err = nvs_erase_key(h, keys[i]);
                err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        s_loaded = false; /* read again from NVS */
        s_have_bad = false;
    }
    unlock();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "%s MHz card, region %s (was %s MHz, %s)%s", c->name, r->name, old_card->name, old->name,
                 lwc_region_tx_enabled(r) ? "" : ": receive only, the radio transmits nothing");
    }
    return err;
}

void radioplan_get(lwc_plan_t *out)
{
    lock();
    load();
    *out = s_plan;
    unlock();
}

radioplan_src_t radioplan_source(void)
{
    lock();
    load();
    const radioplan_src_t s = s_src;
    unlock();
    return s;
}

const char *radioplan_source_name(radioplan_src_t s)
{
    return s == RADIOPLAN_SERVER ? "server" : s == RADIOPLAN_LOCAL ? "local" : "default";
}

static esp_err_t write(const lwc_plan_t *p, radioplan_src_t src)
{
    uint8_t b[LWC_PLAN_PACKED];
    if (!lwc_plan_pack(p, b, sizeof b)) {
        return ESP_FAIL;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_PLAN, b, sizeof b);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_SRC, (uint8_t)src);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

void radioplan_reject_current(void)
{
    lock();
    load();
    s_bad = s_plan;
    s_have_bad = true;
    const esp_err_t err = write(&s_prev, s_prev_src);
    s_plan = s_prev;
    s_src = s_prev_src;
    unlock();
    ESP_LOGE(TAG, "channel plan refused by the radio: back to the %s plan%s", radioplan_source_name(s_prev_src),
             err == ESP_OK ? "" : " (not stored)");
}

esp_err_t radioplan_store(const lwc_plan_t *p, radioplan_src_t src, bool *changed)
{
    ESP_RETURN_ON_FALSE(p != NULL && src <= RADIOPLAN_SERVER, ESP_ERR_INVALID_ARG, TAG, "arg");
    lock();
    load();
    const lwc_plan_err_t e = lwc_plan_check(p, s_region, s_card);
    if (e != LWC_PLAN_OK) {
        unlock();
        ESP_LOGW(TAG, "plan rejected (%d)", (int)e);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_have_bad && lwc_plan_equal(p, &s_bad)) {
        unlock();
        if (changed != NULL) {
            *changed = false;
        }
        return ESP_ERR_INVALID_STATE;
    }
    const bool diff = !lwc_plan_equal(p, &s_plan) || src != s_src;
    esp_err_t err = ESP_OK;
    if (diff) {
        err = write(p, src);
        if (err == ESP_OK) {
            s_prev = s_plan;
            s_prev_src = s_src;
            s_plan = *p;
            s_src = src;
        }
    }
    unlock();
    if (changed != NULL) {
        *changed = diff && err == ESP_OK;
    }
    return err;
}

void radioplan_tx(radio_tx_cfg_t *out)
{
    lock();
    load();
    *out = s_tx;
    unlock();
}

esp_err_t radioplan_set_tx(const radio_tx_cfg_t *c)
{
    ESP_RETURN_ON_FALSE(c != NULL && c->gain_db10 >= RADIO_GAIN_MIN_DB10 && c->gain_db10 <= RADIO_GAIN_MAX_DB10,
                        ESP_ERR_INVALID_ARG, TAG, "gain");
    ESP_RETURN_ON_FALSE(!c->tx_on || CONFIG_GW_TX_ENABLE, ESP_ERR_NOT_SUPPORTED, TAG, "TX not built in");
    lock();
    load();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i16(h, KEY_GAIN, c->gain_db10);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_TX, c->tx_on ? 1U : 0U);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_LNS_TX, c->lns_tx ? 1U : 0U);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    const radio_tx_cfg_t old = s_tx;
    if (err == ESP_OK) {
        s_tx = *c;
    }
    unlock();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "transmit settings: antenna gain %d.%d dBi, gateway TX %s, local server TX %s (were %d.%d dBi, %s, %s)",
                 c->gain_db10 / 10, abs(c->gain_db10 % 10), c->tx_on ? "on" : "off", c->lns_tx ? "on" : "off",
                 old.gain_db10 / 10, abs(old.gain_db10 % 10), old.tx_on ? "on" : "off", old.lns_tx ? "on" : "off");
    }
    return err;
}

void radioplan_rx(radio_rx_cfg_t *out)
{
    lock();
    load();
    *out = s_rx;
    unlock();
}

esp_err_t radioplan_set_rx(const radio_rx_cfg_t *c)
{
    ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_INVALID_ARG, TAG, "arg");
    lock();
    load();
    const lwc_region_t *rg = s_region;
    if (!lwc_ns_rx_ok(rg, c->rx1_delay_s, c->rx2_hz, c->rx2_dr) ||
        (rg->id == LWC_REGION_EU868 && lwc_dc_band(c->rx2_hz) < 0)) {
        unlock();
        ESP_LOGW(TAG, "receive windows rejected");
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_RX1, c->rx1_delay_s);
        if (err == ESP_OK) {
            err = nvs_set_u32(h, KEY_RX2_HZ, c->rx2_hz);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_RX2_DR, c->rx2_dr);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        s_rx = *c;
    }
    unlock();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "local receive windows for new joins: RX1 %u s, RX2 %" PRIu32 " Hz DR%u", (unsigned)c->rx1_delay_s,
                 c->rx2_hz, (unsigned)c->rx2_dr);
    }
    return err;
}

int radioplan_tx_cap_dbm(void)
{
    radio_tx_cfg_t c;
    radioplan_tx(&c);
    int d = EIRP_LIMIT_DB10 - c.gain_db10; /* 0.1 dB */
    int cap = d >= 0 ? d / 10 : -((-d + 9) / 10); /* rounded down */
    return cap < CONFIG_GW_TX_MAX_DBM ? cap : CONFIG_GW_TX_MAX_DBM;
}

static void chan_json(char *out, size_t cap, size_t *o, const lwc_plan_t *p, const lwc_plan_chan_t *c, bool comma)
{
    const int n = snprintf(out + *o, cap - *o, "{\"enable\":%s,\"radio\":%u,\"if\":%" PRId32 ",\"hz\":%" PRIu32 "}%s",
                           c->enable ? "true" : "false", (unsigned)c->radio, c->if_hz, lwc_plan_hz(p, c),
                           comma ? "," : "");
    *o = (n < 0 || (size_t)n >= cap - *o) ? cap : *o + (size_t)n;
}

size_t radioplan_json(char *out, size_t cap)
{
    lwc_plan_t p;
    radioplan_get(&p);
    const radioplan_src_t src = radioplan_source();
    size_t o = 0;
    const lwc_region_t *rg = radioplan_region();
    const lwc_card_t *card = radioplan_card();
    int n = snprintf(out, cap,
                     "{\"card\":\"%s\",\"card_radio\":\"%s\",\"region\":\"%s\",\"region_tx\":%s,\"fixed\":%s,"
                     "\"local\":%s,\"subband\":%d,\"subbands\":%u,\"cards\":{",
                     card->name, card->radio == LWC_RADIO_SX1255 ? "SX1255" : "SX1257", rg->name,
                     lwc_region_tx_enabled(rg) ? "true" : "false", rg->fixed ? "true" : "false",
                     lwc_ns_region_ok(rg) ? "true" : "false",
                     lwc_plan_subband_of(&p, rg) + 1, (unsigned)lwc_region_subbands(rg));
    o = (n < 0 || (size_t)n >= cap) ? cap : (size_t)n;
    /* "cards": {"868": ["EU868", ...], ...}: the regions each card can serve */
    for (int c = 0; c < (int)LWC_CARDS && o < cap; c++) {
        const lwc_card_t *cd = lwc_card((lwc_card_id_t)c);
        n = snprintf(out + o, cap - o, "%s\"%s\":[", c > 0 ? "," : "", cd->name);
        o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
        bool first = true;
        for (int i = 0; i < (int)LWC_REGIONS && o < cap; i++) {
            const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
            if (!lwc_card_region_ok(cd, r)) {
                continue;
            }
            n = snprintf(out + o, cap - o, "%s\"%s\"", first ? "" : ",", r->name);
            o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
            first = false;
        }
        if (o < cap) {
            n = snprintf(out + o, cap - o, "]");
            o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
        }
    }
    if (o < cap) {
        n = snprintf(out + o, cap - o, "},\"source\":\"%s\",\"radios\":[%" PRIu32 ",%" PRIu32 "],\"multisf\":[",
                     radioplan_source_name(src), p.radio_hz[0], p.radio_hz[1]);
        o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
    }
    for (size_t i = 0; i < LWC_PLAN_MULTISF && o < cap; i++) {
        chan_json(out, cap, &o, &p, &p.multisf[i], i + 1U < LWC_PLAN_MULTISF);
    }
    if (o < cap) {
        n = snprintf(out + o, cap - o, "],\"std\":");
        o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
    }
    if (o < cap) {
        chan_json(out, cap, &o, &p, &p.std, false);
    }
    if (o < cap) {
        n = snprintf(out + o, cap - o, ",\"std_sf\":%u,\"std_bw_khz\":%u,\"fsk\":", (unsigned)p.std_sf,
                     (unsigned)p.std_bw_khz);
        o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
    }
    if (o < cap) {
        chan_json(out, cap, &o, &p, &p.fsk, false);
    }
    if (o < cap) {
        radio_tx_cfg_t tx;
        radioplan_tx(&tx);
        radio_rx_cfg_t rx;
        radioplan_rx(&rx);
        n = snprintf(out + o, cap - o,
                     ",\"gain_db10\":%d,\"cap_dbm\":%d,\"ceiling_dbm\":%d,\"tx\":%s,\"lns_tx\":%s,\"tx_built\":%s,"
                     "\"rx1_delay_s\":%u,\"rx2_hz\":%" PRIu32 ",\"rx2_dr\":%u}",
                     tx.gain_db10, radioplan_tx_cap_dbm(), CONFIG_GW_TX_MAX_DBM, tx.tx_on ? "true" : "false",
                     tx.lns_tx ? "true" : "false", CONFIG_GW_TX_ENABLE ? "true" : "false", (unsigned)rx.rx1_delay_s,
                     rx.rx2_hz, (unsigned)rx.rx2_dr);
        o = (n < 0 || (size_t)n >= cap - o) ? cap : o + (size_t)n;
    }
    return o < cap ? o : 0;
}
