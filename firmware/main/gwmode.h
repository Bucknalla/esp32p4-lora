/* Gateway mode (ADR-0015): TTN, standalone or hybrid. Read once at start-up;
 * a change applies after a restart. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lwc/ns.h"

/** The mode for this boot (NVS "pktfwd"/"mode"; migrated from "ttn" on/off). */
lwc_mode_t gwmode(void);

/** True when frames go upstream to the network server (TTN or hybrid). */
bool gwmode_forwards(void);

/** True when the local network server runs (standalone or hybrid). */
bool gwmode_local(void);

const char *gwmode_name(lwc_mode_t m);

/** Parse "ttn", "standalone" or "hybrid". */
bool gwmode_parse(const char *s, lwc_mode_t *out);

/** Store the mode for the next boot. */
esp_err_t gwmode_set(lwc_mode_t m);

/* ---- upstream link (ADR-0016): LoRa Basics Station ------------------------ */

/** Gateway EUI as 16 upper-case hex digits: CONFIG_GW_EUI, or from the base MAC
 *  (FF FE in the middle). */
esp_err_t gwmode_eui(char out[17]);

#define GW_LBS_HOST_MAX 64
#define GW_LBS_KEY_MAX 160

/** Basics Station server host name (default CONFIG_GW_LBS_HOST). */
void gwmode_lbs_host(char out[GW_LBS_HOST_MAX]);
/** Basics Station server port (default CONFIG_GW_LBS_PORT; TTN uses 8887). */
uint16_t gwmode_lbs_port(void);
/** True when an LNS API key is stored (the key itself never leaves NVS except to the link). */
bool gwmode_lbs_key_set(void);
/** The stored LNS API key; for the Basics Station client only, never to be logged. */
esp_err_t gwmode_lbs_key(char out[GW_LBS_KEY_MAX]);

/**
 * Store the Basics Station server for the next boot. NULL (0 for the port)
 * leaves a value as it is. `host`: a host name (letters, digits, '.', '-');
 * `key`: printable, no spaces or quotes; an empty key erases it.
 */
esp_err_t gwmode_set_lbs(const char *host, uint16_t port, const char *key);
