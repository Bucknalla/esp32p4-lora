#include "lwc/mqtt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lwc/hex.h"
#include "lwc/json.h"

bool lwc_mqtt_id_ok(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        if (n >= LWC_MQTT_ID_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return n > 0U;
}

void lwc_mqtt_node_id(uint64_t dev_eui, char out[17])
{
    (void)snprintf(out, 17, "%016" PRIx64, dev_eui);
}

void lwc_mqtt_iso8601(int64_t unix_s, char out[21])
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

bool lwc_mqtt_topic(char *out, size_t cap, const char *gw, const char *node, const char *leaf)
{
    if (out == NULL || cap == 0U || !lwc_mqtt_id_ok(gw) || (node != NULL && !lwc_mqtt_id_ok(node)) || leaf == NULL) {
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

static size_t done(const lwc_sb_t *b)
{
    return b->overflow ? 0U : b->len;
}

/* Fields shared by the uplink and join messages, without the closing brace. */
static void common(lwc_sb_t *b, const lwc_mqtt_up_t *u)
{
    lwc_sb_printf(b, "{\"node_id\":");
    jstr(b, u->node);
    lwc_sb_printf(b, ",\"name\":");
    jstr(b, u->name);
    lwc_sb_printf(b, ",\"dev_eui\":\"%016" PRIX64 "\",\"dev_addr\":\"%08" PRIX32 "\"", u->dev_eui, u->dev_addr);
}

static void radio(lwc_sb_t *b, const lwc_mqtt_up_t *u)
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
        lwc_mqtt_iso8601(u->time_unix_s, ts);
        lwc_sb_printf(b, ",\"ts\":\"%s\"", ts);
    }
}

size_t lwc_mqtt_up_json(const lwc_mqtt_up_t *u, char *out, size_t cap)
{
    if (u == NULL || out == NULL || cap == 0U || !lwc_mqtt_id_ok(u->node) || (u->len > 0U && u->data == NULL)) {
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

size_t lwc_mqtt_join_json(const lwc_mqtt_up_t *u, char *out, size_t cap)
{
    if (u == NULL || out == NULL || cap == 0U || !lwc_mqtt_id_ok(u->node)) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    common(&b, u);
    radio(&b, u);
    lwc_sb_printf(&b, "}");
    return done(&b);
}

bool lwc_mqtt_down_topic(const char *topic, size_t n, const char *gw, uint64_t *dev_eui)
{
    if (topic == NULL || dev_eui == NULL || !lwc_mqtt_id_ok(gw)) {
        return false;
    }
    const size_t g = strlen(gw);
    /* "lora/" gw "/" 16 hex "/down" */
    if (n != 5U + g + 1U + 16U + 5U || memcmp(topic, "lora/", 5) != 0 || memcmp(topic + 5, gw, g) != 0 ||
        topic[5U + g] != '/' || memcmp(topic + 5U + g + 17U, "/down", 5) != 0) {
        return false;
    }
    return lwc_hex_eui(topic + 5U + g + 1U, 16, dev_eui);
}

const char *lwc_mqtt_down_parse(const char *json, size_t n, lwc_mqtt_down_t *out)
{
    if (json == NULL || out == NULL) {
        return "no message";
    }
    memset(out, 0, sizeof *out);
    lwc_jr_t r = {.p = json, .end = json + n};
    bool has_port = false, has_data = false;
    const char *err = NULL; /* the first wrong field; reading goes on, so a later "id" is still kept */
    if (!lwc_jr_lit(&r, '{')) {
        return "not a JSON object";
    }
    if (!lwc_jr_lit(&r, '}')) {
        do {
            const char *k, *v;
            size_t kn, vn;
            if (!lwc_jr_str(&r, &k, &kn) || !lwc_jr_lit(&r, ':')) {
                return "bad JSON";
            }
            const char *bad = NULL;
            if (lwc_jr_key_is(k, kn, "port")) {
                int64_t port;
                if (lwc_jr_num(&r, &v, &vn)) {
                    if (lwc_jr_i64(v, vn, &port) && port >= 1 && port <= 223) {
                        out->port = (uint8_t)port;
                        has_port = true;
                    } else {
                        bad = "port: 1 to 223";
                    }
                } else if (lwc_jr_skip(&r)) {
                    bad = "port: 1 to 223";
                } else {
                    return "bad JSON";
                }
            } else if (lwc_jr_key_is(k, kn, "payload_hex")) {
                if (lwc_jr_str(&r, &v, &vn)) {
                    if (vn % 2U == 0U && vn / 2U <= sizeof out->data &&
                        (vn == 0U || lwc_hex_decode(v, vn, out->data, vn / 2U))) {
                        out->len = vn / 2U;
                        has_data = true;
                    } else {
                        bad = "payload_hex: an even number of hex digits, at most 242 bytes";
                    }
                } else if (lwc_jr_skip(&r)) {
                    bad = "payload_hex: an even number of hex digits, at most 242 bytes";
                } else {
                    return "bad JSON";
                }
            } else if (lwc_jr_key_is(k, kn, "id")) {
                if (lwc_jr_str(&r, &v, &vn)) {
                    if (lwc_dlq_id_ok(v, vn)) {
                        memcpy(out->id, v, vn);
                        out->id[vn] = '\0';
                    } else {
                        bad = "id: 1 to 36 of A-Z a-z 0-9 _ . : -";
                    }
                } else if (lwc_jr_skip(&r)) {
                    bad = "id: 1 to 36 of A-Z a-z 0-9 _ . : -";
                } else {
                    return "bad JSON";
                }
            } else if (lwc_jr_key_is(k, kn, "confirmed")) {
                if (lwc_jr_word(&r, "true")) {
                    out->confirmed = true;
                } else if (lwc_jr_word(&r, "false")) {
                    out->confirmed = false;
                } else if (lwc_jr_skip(&r)) {
                    bad = "confirmed: true or false";
                } else {
                    return "bad JSON";
                }
            } else if (!lwc_jr_skip(&r)) {
                return "bad JSON";
            }
            if (err == NULL) {
                err = bad;
            }
        } while (lwc_jr_lit(&r, ','));
        if (!lwc_jr_lit(&r, '}')) {
            return "bad JSON";
        }
    }
    lwc_jr_ws(&r);
    if (r.p != r.end) {
        return "bad JSON";
    }
    return err != NULL ? err : !has_port ? "port: required" : !has_data ? "payload_hex: required" : NULL;
}

size_t lwc_mqtt_result_json(const lwc_mqtt_result_t *r, char *out, size_t cap)
{
    static const char *const STATES[LWC_MQTT_DL_STATES] = {"queued", "refused", "sent", "acked", "nacked", "dropped"};
    if (r == NULL || out == NULL || cap == 0U || !lwc_mqtt_id_ok(r->node) || (unsigned)r->state >= LWC_MQTT_DL_STATES ||
        (r->id != NULL && r->id[0] != '\0' && !lwc_dlq_id_ok(r->id, strlen(r->id)))) {
        return 0;
    }
    lwc_sb_t b = {.p = out, .cap = cap};
    out[0] = '\0';
    lwc_sb_printf(&b, "{\"node_id\":");
    jstr(&b, r->node);
    lwc_sb_printf(&b, ",\"dev_eui\":\"%016" PRIX64 "\"", r->dev_eui);
    if (r->id != NULL && r->id[0] != '\0') {
        lwc_sb_printf(&b, ",\"id\":\"%s\"", r->id); /* checked: nothing to escape */
    }
    lwc_sb_printf(&b, ",\"status\":\"%s\"", STATES[r->state]);
    if (r->reason != NULL) {
        lwc_sb_printf(&b, ",\"reason\":");
        jstr(&b, r->reason);
    }
    if (r->port >= 0) {
        lwc_sb_printf(&b, ",\"port\":%d", r->port);
    }
    if (r->len >= 0) {
        lwc_sb_printf(&b, ",\"len\":%d", r->len);
    }
    if (r->port >= 0) {
        lwc_sb_printf(&b, ",\"confirmed\":%s", r->confirmed ? "true" : "false");
    }
    if (r->has_fcnt) {
        lwc_sb_printf(&b, ",\"fcnt_down\":%" PRIu32, r->fcnt_down);
    }
    if (r->tx != 0U) {
        lwc_sb_printf(&b, ",\"tx\":%u", r->tx);
    }
    if (r->time_unix_s >= 0) {
        char ts[21];
        lwc_mqtt_iso8601(r->time_unix_s, ts);
        lwc_sb_printf(&b, ",\"ts\":\"%s\"", ts);
    }
    lwc_sb_printf(&b, "}");
    return done(&b);
}
