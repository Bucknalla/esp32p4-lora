/* Gateway mode (ADR-0015): TTN, standalone or hybrid. Read once at start-up;
 * a change applies after a restart. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lwc/ns.h"

/** The mode for this boot (NVS "pktfwd"/"mode"; migrated from "ttn" on/off). */
lwc_mode_t gwmode(void);

/** True when frames go upstream over Semtech UDP (TTN or hybrid). */
bool gwmode_forwards(void);

/** True when the local network server runs (standalone or hybrid). */
bool gwmode_local(void);

const char *gwmode_name(lwc_mode_t m);

/** Parse "ttn", "standalone" or "hybrid". */
bool gwmode_parse(const char *s, lwc_mode_t *out);

/** Store the mode for the next boot. */
esp_err_t gwmode_set(lwc_mode_t m);
