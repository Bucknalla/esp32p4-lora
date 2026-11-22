/*
 * Software watchdog over every long-lived task (ADR-0009). Each task
 * registers when it enters its loop, with a budget that covers its worst
 * legitimate block (DNS, TLS, concentrator start), and beats while it makes
 * progress. The supervisor task feeds the hardware task watchdog itself.
 * A missed budget aborts (panic -> coredump with every task's stack). After
 * SV_LOOP_LIMIT abnormal resets in a row it only logs, so the OTA task can
 * still bring a fix.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int sv_id_t; /* -1: not registered (calls are then no-ops) */

/** Watch the calling task: `name` must be a string literal. */
sv_id_t sv_register(const char *name, uint32_t budget_ms);

void sv_beat(sv_id_t id);

/** Stop watching until the next beat. */
void sv_pause(sv_id_t id);

/** vTaskDelay in slices of at most 1 s, beating between them. */
void sv_delay(sv_id_t id, uint32_t ms);

/** "name=bytes" pairs: least free stack seen per watched task. */
void sv_status_line(char *buf, size_t len);

/** Start the supervisor task. */
esp_err_t sv_start(void);
