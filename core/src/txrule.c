#include "lwc/txrule.h"

#include <stddef.h>

/*
 * Sources (ADR-0027), pinned:
 *   LMN = LoRaMac-node v4.7.0 src/mac/region/Region*.{h,c}: device bands
 *         (VerifyRfFreq, *_BANDn duty cycle), LBT threshold and listen time.
 *   BS  = Basics Station v2.0.6 src/s2e.c (gateway duty cycle, default TX
 *         power) and src/sx130xconf.c (gateway LBT).
 *   RP  = RP002 max EIRP (region_tables.inc, max_eirp_dbm).
 * Where two sources differ, the stricter value. A region without a sourced
 * band list has no row (n_bands 0): it may not transmit.
 * Band limits are centre frequencies, [lo, hi); a closed source range
 * [a, b] is written [a, b + 1).
 */
static const lwc_txrule_t k_rules[LWC_REGIONS] = {
    [LWC_REGION_EU868] = {
        .id = LWC_REGION_EU868,
        /* ETSI EN 300 220 sub-bands as in LMN RegionEU868.c (duty) and the
         * firmware since ADR-0007; power: BS 16 dBm, 27 dBm in 869.4-869.65 */
        .n_bands = 6,
        .band = {
            {863000000U, 865000000U, 1U, 16},   /* 0.1 % */
            {865000000U, 868000000U, 10U, 16},  /* 1 % */
            {868000000U, 868600000U, 10U, 16},  /* g1: 1 % */
            {868700000U, 869200000U, 1U, 16},   /* g2: 0.1 % */
            {869400000U, 869650000U, 100U, 27}, /* g3: 10 % (RX2 869.525) */
            {869700000U, 870000000U, 10U, 16},  /* g4: 1 % */
        },
    },
    [LWC_REGION_US915] = {
        .id = LWC_REGION_US915,
        /* LMN 902.3-927.5 MHz, no duty cycle; power: BS 26 dBm (RP 30) */
        .n_bands = 1,
        .band = {{902300000U, 927500001U, 0U, 26}},
    },
    [LWC_REGION_AU915] = {
        .id = LWC_REGION_AU915,
        /* LMN 915.2-927.8 MHz, no duty cycle; power: BS 30 dBm, RP 30 */
        .n_bands = 1,
        .band = {{915200000U, 927800001U, 0U, 30}},
    },
    /* AS923-1: LBT at -80 dBm for 5 ms (LMN and BS agree) and 10 % per
     * channel (BS), but the band depends on the country and no source gives
     * it: no band list. AS923-2/3/4: no gateway source. */
    [LWC_REGION_AS923_1] = {
        .id = LWC_REGION_AS923_1,
        .chan_dc_permille = 100U,
        .lbt = true,
        .lbt_dbm = -80,
        .lbt_us = 5000U,
    },
    [LWC_REGION_AS923_2] = {.id = LWC_REGION_AS923_2},
    [LWC_REGION_AS923_3] = {.id = LWC_REGION_AS923_3},
    [LWC_REGION_AS923_4] = {.id = LWC_REGION_AS923_4},
    [LWC_REGION_IN865] = {
        .id = LWC_REGION_IN865,
        /* LMN 865-867 MHz, no duty cycle; power: RP 30 dBm (BS: no IN865) */
        .n_bands = 1,
        .band = {{865000000U, 867000001U, 0U, 30}},
    },
    [LWC_REGION_KR920] = {
        .id = LWC_REGION_KR920,
        /* LMN 920.9-923.3 MHz; 2 % per channel (BS); LBT: -67 dBm (BS; LMN
         * -65) for 6 ms (LMN; BS 5 ms); power: LMN 10 dBm below 922.1 MHz,
         * 14 dBm from it (BS 23) */
        .n_bands = 2,
        .band = {{920900000U, 922100000U, 0U, 10}, {922100000U, 923300001U, 0U, 14}},
        .chan_dc_permille = 20U,
        .lbt = true,
        .lbt_dbm = -67,
        .lbt_us = 6000U,
    },
    [LWC_REGION_RU864] = {
        .id = LWC_REGION_RU864,
        /* LMN 864-870 MHz, 1 % (RU864_BAND0); power: RP 16 dBm (BS: no RU864) */
        .n_bands = 1,
        .band = {{864000000U, 870000001U, 10U, 16}},
    },
    /* CN470: LMN v4.7.0 has only the RP002-1.0.1+ plans, not the original
     * plan used here: no row. */
    [LWC_REGION_CN470] = {.id = LWC_REGION_CN470},
    [LWC_REGION_EU433] = {
        .id = LWC_REGION_EU433,
        /* LMN 433.175-434.665 MHz, 1 % (EU433_BAND0); power: RP 12 dBm */
        .n_bands = 1,
        .band = {{433175000U, 434665001U, 10U, 12}},
    },
};

const lwc_txrule_t *lwc_txrule(lwc_region_id_t id)
{
    return ((unsigned)id < LWC_REGIONS) ? &k_rules[id] : NULL;
}

int lwc_txrule_band(const lwc_txrule_t *t, uint32_t hz)
{
    if (t == NULL) {
        return -1;
    }
    for (size_t b = 0; b < t->n_bands && b < LWC_TXRULE_BANDS; b++) {
        if (hz >= t->band[b].lo_hz && hz < t->band[b].hi_hz) {
            return (int)b;
        }
    }
    return -1;
}

lwc_txr_t lwc_txrule_check(const lwc_txrule_t *t, uint32_t hz, bool have_lbt, int8_t *max_dbm)
{
    if (t == NULL || t->n_bands == 0U) {
        return LWC_TXR_NO_RULES;
    }
    const int b = lwc_txrule_band(t, hz);
    if (b < 0) {
        return LWC_TXR_FREQ;
    }
    if (t->lbt && !have_lbt) {
        return LWC_TXR_LBT;
    }
    if (max_dbm != NULL) {
        *max_dbm = t->band[b].max_dbm;
    }
    return LWC_TXR_OK;
}

const char *lwc_txr_str(lwc_txr_t r)
{
    switch (r) {
    case LWC_TXR_OK: return "OK";
    case LWC_TXR_NO_RULES: return "NO_RULES";
    case LWC_TXR_FREQ: return "FREQ";
    case LWC_TXR_LBT: return "LBT";
    default: return "?";
    }
}

/* ==== duty cycle ========================================================= */

#define DC_WINDOW_US 3600000000LL

static int64_t capacity(uint32_t permille)
{
    return DC_WINDOW_US * (int64_t)permille / 1000;
}

static void fill(lwc_dutycycle_t *dc, bool full)
{
    for (size_t b = 0; b < LWC_TXRULE_BANDS; b++) {
        dc->tokens_us[b] = full ? capacity(lwc_dc_permille(dc->rule, (int)b)) : 0;
    }
    for (size_t c = 0; c < LWC_DC_CHANNELS; c++) {
        dc->ch_hz[c] = 0;
        dc->ch_tokens_us[c] = 0;
    }
    dc->ch_full = full;
    dc->used = false;
}

void lwc_dc_init(lwc_dutycycle_t *dc, const lwc_txrule_t *t, int64_t now_us)
{
    dc->rule = t;
    dc->last_us = now_us;
    fill(dc, true);
}

uint32_t lwc_dc_permille(const lwc_txrule_t *t, int band)
{
    return (t != NULL && band >= 0 && (size_t)band < t->n_bands && (size_t)band < LWC_TXRULE_BANDS)
               ? t->band[band].dc_permille
               : 0U;
}

static int64_t refilled(int64_t tokens, int64_t elapsed, uint32_t permille)
{
    const int64_t t = tokens + elapsed * (int64_t)permille / 1000;
    const int64_t cap = capacity(permille);
    return t > cap ? cap : t;
}

bool lwc_dc_take(lwc_dutycycle_t *dc, const lwc_txrule_t *t, int64_t now_us, uint32_t freq_hz,
                 uint32_t airtime_us)
{
    if (dc == NULL || t == NULL) {
        return false;
    }
    if (dc->rule != t) { /* the region changed: full budgets only if none was used */
        const bool full = !dc->used;
        dc->rule = t;
        dc->last_us = now_us;
        fill(dc, full);
        dc->used = !full;
    }
    const int b = lwc_txrule_band(t, freq_hz);
    if (b < 0) {
        return false;
    }
    const int64_t elapsed = now_us - dc->last_us;
    if (elapsed > 0) {
        for (size_t i = 0; i < t->n_bands && i < LWC_TXRULE_BANDS; i++) {
            dc->tokens_us[i] = refilled(dc->tokens_us[i], elapsed, t->band[i].dc_permille);
        }
        for (size_t c = 0; c < LWC_DC_CHANNELS; c++) {
            if (dc->ch_hz[c] != 0U) {
                dc->ch_tokens_us[c] = refilled(dc->ch_tokens_us[c], elapsed, t->chan_dc_permille);
            }
        }
        dc->last_us = now_us;
    }
    const uint32_t bp = t->band[b].dc_permille;
    if (bp != 0U && dc->tokens_us[b] < (int64_t)airtime_us) {
        return false;
    }
    if (t->chan_dc_permille != 0U) {
        size_t ch = LWC_DC_CHANNELS;
        for (size_t c = 0; c < LWC_DC_CHANNELS && ch == LWC_DC_CHANNELS; c++) {
            if (dc->ch_hz[c] == freq_hz) {
                ch = c;
            }
        }
        for (size_t c = 0; c < LWC_DC_CHANNELS && ch == LWC_DC_CHANNELS; c++) {
            if (dc->ch_hz[c] == 0U) {
                ch = c;
                dc->ch_hz[c] = freq_hz;
                dc->ch_tokens_us[c] = dc->ch_full ? capacity(t->chan_dc_permille) : 0;
            }
        }
        if (ch == LWC_DC_CHANNELS || dc->ch_tokens_us[ch] < (int64_t)airtime_us) {
            return false; /* the list is full, or the budget is used up */
        }
        dc->ch_tokens_us[ch] -= (int64_t)airtime_us;
    }
    if (bp != 0U) {
        dc->tokens_us[b] -= (int64_t)airtime_us;
    }
    dc->used = true;
    return true;
}
