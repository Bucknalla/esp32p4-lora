#include "lwc/ha.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lwc/json.h"

bool lwc_ha_id_ok(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (n >= LWC_HA_ID_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return n > 0U;
}

void lwc_ha_node_id(uint64_t dev_eui, char out[17])
{
    (void)snprintf(out, 17, "%016" PRIx64, dev_eui);
}

void lwc_ha_iso8601(int64_t unix_s, char out[21])
{
    int64_t days = unix_s / 86400;
    int64_t sec = unix_s % 86400;
    if (sec < 0) {
        sec += 86400;
        days--;
    }
    /* civil_from_days (Howard Hinnant), proleptic Gregorian */
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char t[48]; /* room for any int, so no truncation warning; the result is 20 characters */
    (void)snprintf(t, sizeof t, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)(y % 10000), (int)m, (int)d, (int)(sec / 3600),
                   (int)(sec / 60 % 60), (int)(sec % 60));
    memcpy(out, t, 20);
    out[20] = '\0';
}

bool lwc_ha_topic(char *out, size_t cap, const char *gw, const char *node, const char *leaf)
{
    if (out == NULL || cap == 0U || !lwc_ha_id_ok(gw) || (node != NULL && !lwc_ha_id_ok(node)) || leaf == NULL) {
        return false;
    }
    const int n = node != NULL ? snprintf(out, cap, "lora/%s/%s/%s", gw, node, leaf)
                               : snprintf(out, cap, "lora/%s/%s", gw, leaf);
    return n > 0 && (size_t)n < cap;
}

/* JSON string: quotes, backslashes and control characters escaped; bytes
 * >= 0x80 pass through (the web form takes UTF-8). */
static void jstr(lwc_sb_t *b, const char *s)
{
    lwc_sb_printf(b, "\"");
    for (; s != NULL && *s != '\0'; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            lwc_sb_printf(b, "\\%c", c);
        } else if (c < 0x20U) {
            lwc_sb_printf(b, "\\u%04x", c);
        } else {
            lwc_sb_printf(b, "%c", c);
        }
    }
    lwc_sb_printf(b, "\"");
}

static size_t done(lwc_sb_t *b)
{
    return b->overflow ? 0U : b->len;
}

/* Fields shared by the uplink and join messages, without the closing brace. */
static void common(lwc_sb_t *b, const lwc_ha_up_t *u)
{
    lwc_sb_printf(b, "{\"node_id\":");
    jstr(b, u->node);
    lwc_sb_printf(b, ",\"name\":");
    jstr(b, u->name);
    lwc_sb_printf(b, ",\"dev_eui\":\"%016" PRIX64 "\",\"dev_addr\":\"%08" PRIX32 "\"", u->dev_eui, u->dev_addr);
}

static void radio(lwc_sb_t *b, const lwc_ha_up_t *u)
{
    lwc_sb_printf(b, ",\"rssi\":%d,\"snr\":%.1f,\"sf\":%u", u->rssi, (double)u->snr, u->sf);
    if (u->bw_khz != 0U) {
        lwc_sb_printf(b, ",\"bw_khz\":%u", u->bw_khz);
    }
    if (u->freq_hz != 0U) {
        lwc_sb_printf(b, ",\"freq_hz\":%" PRIu32, u->freq_hz);
    }
    if (u->time_unix_s >= 0) {
        char ts[21];
        lwc_ha_iso8601(u->time_unix_s, ts);
        lwc_sb_printf(b, ",\"ts\":\"%s\"", ts);
    }
}

size_t lwc_ha_up_json(const lwc_ha_up_t *u, char *out, size_t cap)
{
    if (u == NULL || out == NULL || cap == 0U || !lwc_ha_id_ok(u->node) || (u->len > 0U && u->data == NULL)) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    common(&b, u);
    lwc_sb_printf(&b, ",\"fcnt\":%" PRIu32, u->fcnt);
    if (u->port >= 0) {
        lwc_sb_printf(&b, ",\"port\":%d", u->port);
    }
    lwc_sb_printf(&b, ",\"confirmed\":%s,\"payload_hex\":\"", u->confirmed ? "true" : "false");
    for (size_t i = 0; i < u->len; i++) {
        lwc_sb_printf(&b, "%02X", u->data[i]);
    }
    lwc_sb_printf(&b, "\"");
    radio(&b, u);
    lwc_sb_printf(&b, "}");
    return done(&b);
}

size_t lwc_ha_join_json(const lwc_ha_up_t *u, char *out, size_t cap)
{
    if (u == NULL || out == NULL || cap == 0U || !lwc_ha_id_ok(u->node)) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    common(&b, u);
    radio(&b, u);
    lwc_sb_printf(&b, "}");
    return done(&b);
}

static const struct {
    const char *key;
    const char *label;
    const char *device_class; /* NULL: none */
    const char *unit;
} FIELDS[LWC_HA_FIELDS] = {
    [LWC_HA_RSSI] = {"rssi", "RSSI", "signal_strength", "dBm"},
    [LWC_HA_SNR] = {"snr", "SNR", NULL, "dB"},
};

size_t lwc_ha_sensor_config(const char *gw, const char *node, const char *name, lwc_ha_field_t f, uint32_t expire_s,
                            char *topic, size_t tcap, char *out, size_t cap)
{
    if ((unsigned)f >= (unsigned)LWC_HA_FIELDS || !lwc_ha_id_ok(gw) || !lwc_ha_id_ok(node) || topic == NULL) {
        return 0;
    }
    const int tn = snprintf(topic, tcap, "homeassistant/sensor/%s_%s/config", node, FIELDS[f].key);
    if (tn <= 0 || (size_t)tn >= tcap) {
        return 0;
    }
    if (out == NULL) {
        return 1; /* topic only */
    }
    if (cap == 0U) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    lwc_sb_printf(&b,
                  "{\"name\":\"%s\",\"unique_id\":\"lora_%s_%s\",\"state_topic\":\"lora/%s/%s/up\","
                  "\"value_template\":\"{{ value_json.%s }}\"",
                  FIELDS[f].label, node, FIELDS[f].key, gw, node, FIELDS[f].key);
    if (FIELDS[f].device_class != NULL) {
        lwc_sb_printf(&b, ",\"device_class\":\"%s\"", FIELDS[f].device_class);
    }
    lwc_sb_printf(&b,
                  ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\","
                  "\"availability_topic\":\"lora/%s/status\",\"payload_available\":\"online\","
                  "\"payload_not_available\":\"offline\"",
                  FIELDS[f].unit, gw);
    if (expire_s != 0U) {
        lwc_sb_printf(&b, ",\"expire_after\":%" PRIu32, expire_s);
    }
    lwc_sb_printf(&b, ",\"device\":{\"identifiers\":[\"lora_%s\"],\"name\":", node);
    /* "LoRa <name>", or "LoRa <node id>" for a device without a name */
    char dn[64];
    (void)snprintf(dn, sizeof dn, "LoRa %s", name != NULL && name[0] != '\0' ? name : node);
    jstr(&b, dn);
    lwc_sb_printf(&b, ",\"via_device\":\"lora_gw_%s\",\"manufacturer\":\"Experimental\",\"model\":\"LoRa node\"}}",
                  gw);
    return done(&b);
}

size_t lwc_ha_gateway_config(const char *gw, char *topic, size_t tcap, char *out, size_t cap)
{
    if (!lwc_ha_id_ok(gw) || topic == NULL || out == NULL || cap == 0U) {
        return 0;
    }
    const int tn = snprintf(topic, tcap, "homeassistant/binary_sensor/lora_gw_%s_status/config", gw);
    if (tn <= 0 || (size_t)tn >= tcap) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    lwc_sb_printf(&b,
                  "{\"name\":\"Status\",\"unique_id\":\"lora_gw_%s_status\",\"state_topic\":\"lora/%s/status\","
                  "\"payload_on\":\"online\",\"payload_off\":\"offline\",\"device_class\":\"connectivity\","
                  "\"entity_category\":\"diagnostic\","
                  "\"device\":{\"identifiers\":[\"lora_gw_%s\"],\"name\":\"LoRa gateway %s\","
                  "\"manufacturer\":\"Experimental\",\"model\":\"ESP32-P4 LoRaWAN gateway\"}}",
                  gw, gw, gw, gw);
    return done(&b);
}
