#include "lwc/region.h"

#include <string.h>

#include "region_tables.inc"

const lwc_region_t *lwc_region(lwc_region_id_t id)
{
    return (unsigned)id < (unsigned)LWC_REGIONS ? REGIONS[id] : NULL;
}

bool lwc_region_parse(const char *name, lwc_region_id_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        if (strcmp(REGIONS[i]->name, name) == 0) {
            *out = (lwc_region_id_t)i;
            return true;
        }
    }
    return false;
}

int lwc_region_dr_of(const lwc_region_t *r, uint8_t sf, uint16_t bw_khz, bool downlink)
{
    if (r == NULL) {
        return -1;
    }
    /* US915, AU915: uplink DR0-6, downlink DR8-13 share SF/BW pairs. CN470:
     * the same DRs both ways. */
    const bool split = r->fixed && r->down.min_dr >= 8U;
    const int from = split && downlink ? 8 : 0;
    const int to = split && !downlink ? 8 : 16;
    for (int i = from; i < to; i++) {
        const lwc_region_dr_t *d = &r->dr[i];
        if (d->kind == LWC_DR_LORA && d->sf == sf && d->bw_khz == bw_khz) {
            return i;
        }
    }
    return -1;
}

size_t lwc_region_max_n(const lwc_region_t *r, int dr, bool dwell)
{
    if (r == NULL || dr < 0 || dr > 15 || r->dr[dr].kind == LWC_DR_NONE) {
        return 0;
    }
    return dwell ? r->dr[dr].n_dwell : r->dr[dr].n;
}

int lwc_region_rx1_dr(const lwc_region_t *r, int up_dr, uint8_t offset, bool dwell)
{
    if (r == NULL || up_dr < 0 || up_dr > 15 || offset > 7U) {
        return -1;
    }
    const int dr = r->rx1_dr[up_dr][offset];
    return dr >= 0 && dwell && dr < (int)r->rx1_dwell_min_dr ? (int)r->rx1_dwell_min_dr : dr;
}

/* index of hz on grid g (0..n-1), or -1 */
static int on_grid(const lwc_region_grid_t *g, uint32_t hz, int n)
{
    if (g->step_hz == 0U || hz < g->base_hz) {
        return -1;
    }
    const uint32_t off = hz - g->base_hz;
    if (off % g->step_hz != 0U || off / g->step_hz >= (uint32_t)n) {
        return -1;
    }
    return (int)(off / g->step_hz);
}

int lwc_region_channel(const lwc_region_t *r, uint32_t hz, uint16_t bw_khz)
{
    if (r == NULL || !r->fixed) {
        return -1;
    }
    if (bw_khz == 125U) {
        return on_grid(&r->up125, hz, r->n_up125);
    }
    if (bw_khz == 500U && r->n_up500 > 0U) {
        const int i = on_grid(&r->up500, hz, r->n_up500);
        return i < 0 ? -1 : r->n_up125 + i;
    }
    return -1;
}

uint32_t lwc_region_rx1_hz(const lwc_region_t *r, uint32_t up_hz, uint16_t up_bw_khz)
{
    if (r == NULL) {
        return 0;
    }
    if (!r->fixed) {
        return up_hz;
    }
    const int ch = lwc_region_channel(r, up_hz, up_bw_khz);
    return ch < 0 || r->n_down == 0U ? 0U : r->down.base_hz + (uint32_t)(ch % r->n_down) * r->down.step_hz;
}

int8_t lwc_region_tx_power_dbm(const lwc_region_t *r, uint8_t idx)
{
    if (r == NULL || idx >= r->tx_powers) {
        return INT8_MIN;
    }
    return (int8_t)(r->max_eirp_dbm - 2 * (int)idx);
}

uint8_t lwc_region_subbands(const lwc_region_t *r)
{
    return r != NULL && r->fixed ? (uint8_t)(r->n_up125 / 8U) : 0U;
}

/* US915 and AU915: 64 + 8 uplink channels, 8 sub-bands */
static bool us_au(const lwc_region_t *r)
{
    return r != NULL && r->fixed && r->n_up125 == 64U && r->n_up500 == 8U;
}

bool lwc_region_cflist_subband(const lwc_region_t *r, uint8_t sb, uint8_t out[16])
{
    if (!us_au(r) || sb > 7U || out == NULL) {
        return false;
    }
    memset(out, 0, 16);
    /* ChMask0-3: channels 0-63 (16 each), ChMask4: 64-71; little-endian */
    const unsigned block = sb / 2U;
    const uint16_t m = (uint16_t)(0xFFU << (8U * (sb % 2U)));
    out[2U * block] = (uint8_t)(m & 0xFFU);
    out[2U * block + 1U] = (uint8_t)(m >> 8);
    out[8] = (uint8_t)(1U << sb);
    out[15] = 1; /* CFListType 1: channel mask */
    return true;
}

bool lwc_region_link_adr_subband(const lwc_region_t *r, uint8_t sb, uint16_t mask[2], uint8_t cntl[2])
{
    if (!us_au(r) || sb > 7U || mask == NULL || cntl == NULL) {
        return false;
    }
    mask[0] = (uint16_t)(1U << sb); /* 64 + sb */
    cntl[0] = 7;                    /* all 125 kHz channels off */
    mask[1] = (uint16_t)(0xFFU << (8U * (sb % 2U)));
    cntl[1] = (uint8_t)(sb / 2U);   /* the block of channels 16*cntl..16*cntl+15 */
    return true;
}

bool lwc_region_tx_enabled(const lwc_region_t *r)
{
    return r != NULL && r->id == LWC_REGION_EU868;
}

/* ---- Concentrator cards ------------------------------------------------------ */

/* Bands: 868 the EU 863-870 MHz band (as before); 915 the US 902-928 MHz band;
 * 433 the 433.05-434.79 MHz band and 470 the 470-510 MHz band (TTN frequency
 * plans EU_433, CN_470_510). RSSI offsets: 868 the Nebra EU868 value used so
 * far; the others from the TTN frequency plans (SX1257 at 915: -166, SX1255:
 * -176). Neither is calibrated for a given card. */
static const lwc_card_t CARDS[LWC_CARDS] = {
    [LWC_CARD_868] = {LWC_CARD_868, "868", 863000000U, 870000000U, LWC_RADIO_SX1257, -158},
    [LWC_CARD_915] = {LWC_CARD_915, "915", 902000000U, 928000000U, LWC_RADIO_SX1257, -166},
    [LWC_CARD_433] = {LWC_CARD_433, "433", 433050000U, 434790000U, LWC_RADIO_SX1255, -176},
    [LWC_CARD_470] = {LWC_CARD_470, "470", 470000000U, 510000000U, LWC_RADIO_SX1255, -176},
};

const lwc_card_t *lwc_card(lwc_card_id_t id)
{
    return (unsigned)id < (unsigned)LWC_CARDS ? &CARDS[id] : NULL;
}

bool lwc_card_parse(const char *name, lwc_card_id_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < (int)LWC_CARDS; i++) {
        if (strcmp(CARDS[i].name, name) == 0) {
            *out = (lwc_card_id_t)i;
            return true;
        }
    }
    return false;
}

bool lwc_card_fits(const lwc_card_t *c, uint32_t hz, uint32_t bw_hz)
{
    return c != NULL && hz >= bw_hz / 2U && hz - bw_hz / 2U >= c->lo_hz && (uint64_t)hz + bw_hz / 2U <= c->hi_hz;
}

static bool grid_fits(const lwc_card_t *c, const lwc_region_grid_t *g, uint8_t n, uint32_t bw_hz)
{
    return n == 0U || (lwc_card_fits(c, g->base_hz, bw_hz) &&
                       lwc_card_fits(c, g->base_hz + (uint32_t)(n - 1U) * g->step_hz, bw_hz));
}

bool lwc_card_region_ok(const lwc_card_t *c, const lwc_region_t *r)
{
    if (c == NULL || r == NULL || r->rx2_dr > 15U || r->dr[r->rx2_dr].kind != LWC_DR_LORA) {
        return false;
    }
    if (!lwc_card_fits(c, r->rx2_hz, r->dr[r->rx2_dr].bw_khz * 1000U)) {
        return false;
    }
    if (r->fixed) {
        return grid_fits(c, &r->up125, r->n_up125, 125000U) && grid_fits(c, &r->up500, r->n_up500, 500000U) &&
               grid_fits(c, &r->down, r->n_down, r->dr[r->down.min_dr].bw_khz * 1000U);
    }
    for (size_t k = 0; k < r->n_default; k++) {
        if (!lwc_card_fits(c, r->default_ch[k].hz, 125000U)) {
            return false;
        }
    }
    return r->n_default > 0U;
}

const lwc_card_t *lwc_card_for_region(const lwc_region_t *r)
{
    for (int i = 0; i < (int)LWC_CARDS; i++) {
        if (lwc_card_region_ok(&CARDS[i], r)) {
            return &CARDS[i];
        }
    }
    return NULL;
}
