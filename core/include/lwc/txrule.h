/*
 * The gateway's own transmit rules per region (ADR-0027): where it may
 * transmit, at what power, how often (duty cycle) and whether it must listen
 * first (LBT). Pure, no I/O: host-tested.
 *
 * These rules do not let a region transmit. lwc_region_tx_enabled() is the
 * gate (only EU868); a region with a complete row here is still receive-only
 * until its transmit path is measured and approved.
 *
 * Device dwell time is not here: the server sets it with TxParamSetupReq.
 */
#ifndef LWC_TXRULE_H
#define LWC_TXRULE_H

#include <stdbool.h>
#include <stdint.h>

#include "lwc/region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_TXRULE_BANDS 6U

typedef struct {
    uint32_t lo_hz, hi_hz; /* channel centre frequency, [lo, hi) */
    uint16_t dc_permille;  /* duty cycle per hour, 1 = 0.1 %; 0: no limit */
    int8_t max_dbm;        /* highest TX power in the band */
} lwc_txband_t;

typedef struct {
    lwc_region_id_t id;
    uint8_t n_bands; /* 0: no sourced rules: the region may not transmit */
    lwc_txband_t band[LWC_TXRULE_BANDS];
    uint16_t chan_dc_permille; /* duty cycle per downlink channel; 0: no limit */
    bool lbt;                  /* listen before talk is required */
    int8_t lbt_dbm;            /* the channel is free below this RSSI */
    uint16_t lbt_us;           /* listen this long */
} lwc_txrule_t;

/** The rules of a region. Never NULL for a known region; NULL for an unknown id. */
const lwc_txrule_t *lwc_txrule(lwc_region_id_t id);

/** The band holding `hz`, or -1. */
int lwc_txrule_band(const lwc_txrule_t *t, uint32_t hz);

typedef enum {
    LWC_TXR_OK = 0,
    LWC_TXR_NO_RULES, /* no sourced rules for the region */
    LWC_TXR_FREQ,     /* outside every band */
    LWC_TXR_LBT,      /* LBT is required and the hardware cannot do it */
} lwc_txr_t;

/**
 * May the gateway transmit at `hz`? `have_lbt`: the hardware can listen before
 * talk. On LWC_TXR_OK, `*max_dbm` (if not NULL) is the band's highest power;
 * the caller only lowers its own cap to it.
 */
lwc_txr_t lwc_txrule_check(const lwc_txrule_t *t, uint32_t hz, bool have_lbt, int8_t *max_dbm);

const char *lwc_txr_str(lwc_txr_t r);

/* ---- Duty cycle ----------------------------------------------------------
 * Token bucket over one hour per band (capacity = duty x 3600 s, refilled
 * continuously) and per downlink channel. Times are host monotonic
 * microseconds. */

#define LWC_DC_CHANNELS 16U

typedef struct {
    const lwc_txrule_t *rule;
    int64_t last_us;
    int64_t tokens_us[LWC_TXRULE_BANDS];
    uint32_t ch_hz[LWC_DC_CHANNELS];
    int64_t ch_tokens_us[LWC_DC_CHANNELS];
    bool ch_full; /* a new channel starts with a full budget (not after a region change) */
    bool used;    /* a frame was taken since init: a region change then starts empty */
} lwc_dutycycle_t;

/** Full budgets for the rule's bands. */
void lwc_dc_init(lwc_dutycycle_t *dc, const lwc_txrule_t *t, int64_t now_us);

/** Duty cycle of a band in per-mille (1 = 0.1 %, 10 = 1 %, 100 = 10 %); 0 for none. */
uint32_t lwc_dc_permille(const lwc_txrule_t *t, int band);

/**
 * True and consumed if `airtime_us` fits in the budgets now. False outside
 * every band, or when the per-channel list is full. A different rule than
 * before (the region changed) starts from full budgets if no frame was taken
 * yet, else from empty ones, so a region change cannot refill them.
 */
bool lwc_dc_take(lwc_dutycycle_t *dc, const lwc_txrule_t *t, int64_t now_us, uint32_t freq_hz,
                 uint32_t airtime_us);

#ifdef __cplusplus
}
#endif

#endif /* LWC_TXRULE_H */
