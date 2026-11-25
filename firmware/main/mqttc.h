/* MQTT client (ADR-0018): uplink and join events from the local network
 * server out, downlinks in, and the gateway's online state. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lwc/ns.h"

#define MQTTC_HOST_MAX 64 /* with the NUL */
#define MQTTC_USER_MAX 64
#define MQTTC_PASS_MAX 64
#define MQTTC_GWID_MAX 32

typedef enum {
    MQTTC_UP = 1,
    MQTTC_JOIN,
} mqttc_kind_t;

/** One event from the local network server. Copied into the queue. */
typedef struct {
    uint8_t kind; /* mqttc_kind_t */
    bool confirmed;
    int16_t port; /* -1: no FPort */
    uint64_t dev_eui;
    char name[LWC_NS_NAME_MAX + 1U];
    uint32_t dev_addr;
    uint32_t fcnt;
    int16_t rssi;
    float snr;
    uint8_t sf;
    uint16_t bw_khz;
    uint32_t freq_hz;
    int64_t time_unix_s; /* < 0: wall clock not set */
    uint8_t len;
    uint8_t data[242];
} mqttc_ev_t;

/** Settings for display (no password). */
typedef struct {
    bool on;
    char host[MQTTC_HOST_MAX];
    uint16_t port;
    char user[MQTTC_USER_MAX];
    bool pass_set;
    char gw_id[MQTTC_GWID_MAX]; /* the stored id, or the default (lower-case gateway EUI) */
} mqttc_cfg_t;

void mqttc_get(mqttc_cfg_t *c);

/**
 * Store settings for the next boot. NULL (or 0 for the port) leaves a value as it is. `host`: a host name or IPv4
 * address; `user`/`pass`: printable ASCII; an empty `pass` erases it; an empty
 * `gw_id` returns to the default; otherwise [a-z0-9_], at most 31.
 * ESP_ERR_INVALID_ARG for a value that does not fit these rules.
 */
esp_err_t mqttc_set(const bool *on, const char *host, uint16_t port, const char *user, const char *pass,
                    const char *gw_id);

/** Start the client and its task when MQTT is on and a host is set. */
esp_err_t mqttc_start(void);

/** Queue an event; never blocks. Dropped (and counted) when MQTT is off, not
 *  connected, or the queue is full. */
void mqttc_post(const mqttc_ev_t *ev);

/** state=off|no_host|connecting|connected|refused host=.. port=.. user=.. pass=set|none
 *  gw_id=.. published=N dropped=N failed=N connects=N down_queued=N down_refused=N */
void mqttc_status_line(char *buf, size_t len);
