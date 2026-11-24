/* MQTT to Home Assistant (ADR-0018): uplink and join events from the local
 * network server, gateway online/offline, and MQTT discovery. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lwc/ns.h"

#define MQTTHA_HOST_MAX 64 /* with the NUL */
#define MQTTHA_USER_MAX 64
#define MQTTHA_PASS_MAX 64
#define MQTTHA_GWID_MAX 32
#define MQTTHA_EXPIRE_MAX (7U * 24U * 3600U)

typedef enum {
    MQTTHA_UP = 1,
    MQTTHA_JOIN,
    MQTTHA_REMOVED,
} mqttha_kind_t;

/** One event from the local network server. Copied into the queue. */
typedef struct {
    uint8_t kind; /* mqttha_kind_t */
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
} mqttha_ev_t;

/** Settings for display (no password). */
typedef struct {
    bool on;
    char host[MQTTHA_HOST_MAX];
    uint16_t port;
    char user[MQTTHA_USER_MAX];
    bool pass_set;
    char gw_id[MQTTHA_GWID_MAX]; /* the stored id, or the default (lower-case gateway EUI) */
    uint32_t expire_s;
} mqttha_cfg_t;

void mqttha_get(mqttha_cfg_t *c);

/**
 * Store settings for the next boot. NULL (or 0 for the port, a negative value
 * for `expire_s`) leaves a value as it is. `host`: a host name or IPv4
 * address; `user`/`pass`: printable ASCII; an empty `pass` erases it; an empty
 * `gw_id` returns to the default; otherwise [a-z0-9_], at most 31.
 * ESP_ERR_INVALID_ARG for a value that does not fit these rules.
 */
esp_err_t mqttha_set(const bool *on, const char *host, uint16_t port, const char *user, const char *pass,
                     const char *gw_id, int32_t expire_s);

/** Start the client and its task when MQTT is on and a host is set. */
esp_err_t mqttha_start(void);

/** Queue an event; never blocks. Dropped (and counted) when MQTT is off, not
 *  connected, or the queue is full. */
void mqttha_post(const mqttha_ev_t *ev);

/** state=off|no_host|connecting|connected|refused host=.. port=.. user=.. pass=set|none
 *  gw_id=.. published=N dropped=N failed=N connects=N */
void mqttha_status_line(char *buf, size_t len);
