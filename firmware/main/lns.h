/* Local network server task (ADR-0015): standalone and hybrid modes. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwc/ns.h"

/**
 * Load the device table (every mode, so devices can be added before a mode
 * change). In standalone and hybrid modes, start the task (core 1) reading
 * `radio_q`; in hybrid mode it puts every frame it does not handle on `*fwd_q`
 * (created here) for the forwarder. `*fwd_q` is NULL otherwise.
 */
esp_err_t lns_start(QueueHandle_t radio_q, QueueHandle_t *fwd_q);

bool lns_running(void);

/** Add a device (web API, admin). The AppKey is stored, never shown again. */
lwc_ns_err_t lns_add(uint64_t dev_eui, uint64_t join_eui, const uint8_t app_key[16], lwc_ns_mac_t mac,
                     const char *name, lwc_ns_class_t cls);

/** Change a device's class (A or C) and/or its class C ACK wait (seconds,
 *  0 = default), ADR-0020; NULL leaves one as it is. Its session is kept. */
lwc_ns_err_t lns_set_class(uint64_t dev_eui, const lwc_ns_class_t *cls, const uint16_t *c_ack_s);
lwc_ns_err_t lns_remove(uint64_t dev_eui);

/**
 * Queue an application downlink for a local device (MQTT, ADR-0018); it goes
 * out with the device's next class A answer. `confirmed`: the device must
 * acknowledge it in its next uplink. `id`: "" or the caller's id, echoed in
 * the results. Never waits long (200 ms at most for the lock).
 * ESP_ERR_INVALID_STATE: the local server is not running;
 * ESP_ERR_NOT_SUPPORTED: local transmit is off; ESP_ERR_NOT_FOUND: no such
 * device; ESP_ERR_INVALID_SIZE: larger than the device's RX2 window allows;
 * ESP_ERR_NO_MEM: its queue is full.
 */
esp_err_t lns_queue_down(uint64_t dev_eui, uint8_t port, const uint8_t *data, size_t len, bool confirmed,
                         const char *id);

/** The radio's answer to a local server request (from the TX task; never waits). */
void lns_tx_result(uint16_t token, bool ok);

/** JSON array of devices (no keys) into `out`; returns its length. */
size_t lns_devices_json(char *out, size_t cap);

/** One status line: devices, joins, uplinks, dropped, dry run. */
void lns_status_line(char *buf, size_t len);
