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
                     const char *name);
lwc_ns_err_t lns_remove(uint64_t dev_eui);

/** JSON array of devices (no keys) into `out`; returns its length. */
size_t lns_devices_json(char *out, size_t cap);

/** One status line: devices, joins, uplinks, dropped, dry run. */
void lns_status_line(char *buf, size_t len);
