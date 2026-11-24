/*
 * MQTT messages for Home Assistant (ADR-0018): topics, uplink and join JSON,
 * and MQTT discovery configs. Pure formatting, no I/O, so it runs in host
 * tests. All ids that go into a topic or a unique_id are checked with
 * lwc_ha_id_ok() first ([a-z0-9_] only, so never '/', '+' or '#').
 *
 *   lora/<gw>/status            "online" / "offline" (retained; last will)
 *   lora/<gw>/<node>/up         uplink JSON
 *   lora/<gw>/<node>/join       join JSON
 *   homeassistant/sensor/<node>_<field>/config           retained discovery
 *   homeassistant/binary_sensor/lora_gw_<gw>_status/config
 */
#ifndef LWC_HA_H
#define LWC_HA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_HA_ID_MAX 31U /* characters in a gateway or node id */
#define LWC_HA_TOPIC_MAX 96U

/** True for 1..LWC_HA_ID_MAX characters of [a-z0-9_]. */
bool lwc_ha_id_ok(const char *s);

/** Node id for a device: its DevEUI in lower-case hex (stable: a rename does
 *  not change it, so Home Assistant keeps the same entities). */
void lwc_ha_node_id(uint64_t dev_eui, char out[17]);

/** "YYYY-MM-DDTHH:MM:SSZ" (UTC) from Unix seconds; out[21]. */
void lwc_ha_iso8601(int64_t unix_s, char out[21]);

/** lora/<gw>/<leaf> or lora/<gw>/<node>/<leaf> (node NULL). False if an id is
 *  bad or it does not fit. */
bool lwc_ha_topic(char *out, size_t cap, const char *gw, const char *node, const char *leaf);

typedef struct {
    const char *node;     /* lwc_ha_node_id() */
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
} lwc_ha_up_t;

/** Uplink JSON. Returns the length, 0 if it does not fit. */
size_t lwc_ha_up_json(const lwc_ha_up_t *u, char *out, size_t cap);

/** Join JSON (node, name, dev_eui, dev_addr, rssi, snr, sf, ts). Uses the same
 *  fields of `u`; fcnt, port and data are ignored. */
size_t lwc_ha_join_json(const lwc_ha_up_t *u, char *out, size_t cap);

typedef enum {
    LWC_HA_RSSI,
    LWC_HA_SNR,
    LWC_HA_FIELDS
} lwc_ha_field_t;

/**
 * Discovery config for one value of a node: topic and retained JSON.
 * `expire_s` 0 leaves out expire_after. Returns the JSON length, 0 if an id is
 * bad or it does not fit. With `out` NULL only the topic is written (to clear
 * the config with an empty retained message).
 */
size_t lwc_ha_sensor_config(const char *gw, const char *node, const char *name, lwc_ha_field_t f, uint32_t expire_s,
                            char *topic, size_t tcap, char *out, size_t cap);

/** Discovery config for the gateway's connectivity binary sensor. */
size_t lwc_ha_gateway_config(const char *gw, char *topic, size_t tcap, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LWC_HA_H */
