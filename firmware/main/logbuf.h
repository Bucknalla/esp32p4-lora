/*
 * Log copy in PSRAM (ADR-0009). Every ESP_LOG line is also appended to a
 * ring in PSRAM .noinit memory, which survives software resets and panics.
 * At boot the previous boot's tail is kept (and printed after an abnormal
 * reset) before the ring starts again.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Attach the ring, keep the previous boot's tail, install the log hook.
 *  Call first in app_main. */
void logbuf_init(void);

/** Print the previous boot's last lines (after an abnormal reset). */
void logbuf_print_previous(void);

/** Text kept from the previous boot ("" when none). */
const char *logbuf_previous(void);
