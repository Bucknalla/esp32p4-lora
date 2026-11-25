/*
 * MQTT messages (ADR-0018): topics, uplink and join JSON out, downlink JSON in.
 * Pure formatting and parsing, no I/O, so it runs in host tests. All ids that
 * go into a topic are checked with lwc_mqtt_id_ok() first ([a-z0-9_] only, so
 * never '/', '+' or '#').
 *
 *   lora/<gw>/status            "online" / "offline" (retained; last will)
 *   lora/<gw>/<node>/up         uplink JSON
 *   lora/<gw>/<node>/join       join JSON
 *   lora/<gw>/<node>/down       downlink JSON (in)
 */
#ifndef LWC_MQTT_H
#define LWC_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_MQTT_ID_MAX 31U /* characters in a gateway or node id */
#define LWC_MQTT_TOPIC_MAX 96U

/** True for 1..LWC_MQTT_ID_MAX characters of [a-z0-9_]. */
bool lwc_mqtt_id_ok(const char *s);

/** Node id for a device: its DevEUI in lower-case hex (stable: a rename does
 *  not change its topics). */
void lwc_mqtt_node_id(uint64_t dev_eui, char out[17]);

/** "YYYY-MM-DDTHH:MM:SSZ" (UTC) from Unix seconds; out[21]. */
void lwc_mqtt_iso8601(int64_t unix_s, char out[21]);

/** lora/<gw>/<leaf> or lora/<gw>/<node>/<leaf> (node NULL). False if an id is
 *  bad or it does not fit. */
bool lwc_mqtt_topic(char *out, size_t cap, const char *gw, const char *node, const char *leaf);

typedef struct {
    const char *node;     /* lwc_mqtt_node_id() */
    const char *name;     /* device name from the web form; may be empty; escaped */
    uint64_t dev_eui;
    uint32_t dev_addr;
    uint32_t fcnt;
    int port;             /* -1: no FPort (no payload) */
    bool confirmed;
    const uint8_t *data;  /* decrypted FRMPayload */
    size_t len;
    int rssi;
    float snr;
    unsigned sf;
    unsigned bw_khz;
    uint32_t freq_hz;
    int64_t time_unix_s;  /* < 0: wall clock not set, no "ts" */
} lwc_mqtt_up_t;

/** Uplink JSON. Returns the length, 0 if it does not fit. */
size_t lwc_mqtt_up_json(const lwc_mqtt_up_t *u, char *out, size_t cap);

/** Join JSON (node, name, dev_eui, dev_addr, rssi, snr, sf, ts). Uses the same
 *  fields of `u`; fcnt, port and data are ignored. */
size_t lwc_mqtt_join_json(const lwc_mqtt_up_t *u, char *out, size_t cap);

/* ---- downlinks in: lora/<gw>/<node>/down -------------------------------- */

#define LWC_MQTT_DOWN_MAX 242U /* the largest EU868 FRMPayload (DR4-DR7: 222) fits */

typedef struct {
    uint8_t port; /* 1..223 */
    bool confirmed;
    uint8_t data[LWC_MQTT_DOWN_MAX];
    size_t len;
} lwc_mqtt_down_t;

/**
 * `topic` (not NUL-terminated, `n` bytes) is lora/<gw>/<16 hex digits>/down:
 * sets *dev_eui. False for any other topic.
 */
bool lwc_mqtt_down_topic(const char *topic, size_t n, const char *gw, uint64_t *dev_eui);

/**
 * Parse a downlink message (untrusted): {"port":1..223,"payload_hex":"..",
 * "confirmed":false}. `port` and `payload_hex` are required (hex may be empty);
 * `confirmed` is optional; other keys are skipped. Returns NULL, or what is
 * wrong (for the log).
 */
const char *lwc_mqtt_down_parse(const char *json, size_t n, lwc_mqtt_down_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LWC_MQTT_H */
