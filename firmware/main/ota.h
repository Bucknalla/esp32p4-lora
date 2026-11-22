/*
 * Firmware update and recovery (ADR-0008).
 *
 *   - Boot report: reset reason, running slot, image state, rolled-back
 *     image, stored coredump.
 *   - Pull: every CONFIG_GW_OTA_POLL_MIN minutes (and on OTA_CHECK) read the
 *     image header at CONFIG_GW_OTA_URL; install a new signed image.
 *   - Health check: a new image boots "pending verify"; it is marked valid
 *     when the gateway is healthy, else the bootloader rolls back.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Log why and what we booted. Call early, after NVS init. */
void ota_boot_report(void);

/** True while the running image waits for the health check. */
bool ota_pending_verify(void);

/** For a new (pending-verify) image, start the health check and its
 *  deadline. Call right after ota_boot_report(), before anything that can
 *  hang, so a hang later in start-up still ends in a rollback. */
void ota_health_start(void);

/** Start the update task (checks once an IP is up). Needs esp_netif and
 *  the default event loop. */
esp_err_t ota_start(void);

/** Ask the update task to check now (console OTA_CHECK). */
void ota_check_now(void);

/** One-line summary for the console STATUS command. */
void ota_status_line(char *buf, size_t len);
