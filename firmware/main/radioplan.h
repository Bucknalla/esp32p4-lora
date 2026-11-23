/* The radio's channel plan (ADR-0017): read by the concentrator set-up, the
 * local server's join-accepts and the Basics Station check. Stored in NVS
 * ("radio"/"plan"); the built-in EU868 plan when nothing valid is stored. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lwc/plan.h"

typedef enum {
    RADIOPLAN_DEFAULT = 0, /* built-in EU868 */
    RADIOPLAN_LOCAL = 1,   /* set on the Config tab */
    RADIOPLAN_SERVER = 2,  /* from the network server's router_config */
} radioplan_src_t;

/** The stored plan (any task). */
void radioplan_get(lwc_plan_t *out);
radioplan_src_t radioplan_source(void);
const char *radioplan_source_name(radioplan_src_t s);

/** Check and store a plan for the radio's next start. `*changed` (may be
 *  NULL): it differs from the stored one. ESP_ERR_INVALID_ARG if it fails
 *  lwc_plan_check. */
esp_err_t radioplan_store(const lwc_plan_t *p, radioplan_src_t src, bool *changed);
/* ESP_ERR_INVALID_STATE: this plan was rejected by the radio earlier. */

/** The radio could not start with the stored plan: go back to the one stored
 *  before it, and refuse this plan from now on (until a reboot). */
void radioplan_reject_current(void);

/* ---- transmit settings (ADR-0017 step 4) ------------------------------------------- */

typedef struct {
    int16_t gain_db10; /* antenna gain, 0.1 dBi (default 30: the ~3 dBi Nebra antenna, ADR-0007) */
    bool tx_on;        /* gateway transmit (default CONFIG_GW_TX_ENABLE) */
    bool lns_tx;       /* local server transmit (default CONFIG_GW_LNS_TX: off in releases) */
} radio_tx_cfg_t;

#define RADIO_GAIN_MIN_DB10 (-50)
#define RADIO_GAIN_MAX_DB10 150

void radioplan_tx(radio_tx_cfg_t *out);
/** Store and apply at once (the power cap takes effect at the radio's next start). */
esp_err_t radioplan_set_tx(const radio_tx_cfg_t *c);
/** Power cap at the connector: EU868 16.1 dBm EIRP (14 dBm ERP) minus the
 *  antenna gain, never above CONFIG_GW_TX_MAX_DBM. */
int radioplan_tx_cap_dbm(void);

/* ---- local server receive windows (ADR-0017 step 5) -------------------------------- */

typedef struct {
    uint8_t rx1_delay_s; /* 1..15 */
    uint32_t rx2_hz;     /* in an EU868 sub-band the radio may transmit in */
    uint8_t rx2_dr;      /* DR0..DR5 */
} radio_rx_cfg_t;

void radioplan_rx(radio_rx_cfg_t *out);
/** For new joins only; ESP_ERR_INVALID_ARG if not usable. */
esp_err_t radioplan_set_rx(const radio_rx_cfg_t *c);

/** The plan as JSON for /api/radio; returns the length (0 if it does not fit). */
size_t radioplan_json(char *out, size_t cap);
