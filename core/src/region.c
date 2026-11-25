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
    /* the fixed plans: uplink DR0-6, downlink DR8-13 share SF/BW pairs */
    const int from = r->fixed && downlink ? 8 : 0;
    const int to = r->fixed && !downlink ? 8 : 16;
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
        return on_grid(&r->up125, hz, 64);
    }
    if (bw_khz == 500U) {
        const int i = on_grid(&r->up500, hz, 8);
        return i < 0 ? -1 : 64 + i;
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
    return ch < 0 ? 0U : r->down.base_hz + (uint32_t)(ch % 8) * r->down.step_hz;
}

int8_t lwc_region_tx_power_dbm(const lwc_region_t *r, uint8_t idx)
{
    if (r == NULL || idx >= r->tx_powers) {
        return INT8_MIN;
    }
    return (int8_t)(r->max_eirp_dbm - 2 * (int)idx);
}

bool lwc_region_cflist_subband(const lwc_region_t *r, uint8_t sb, uint8_t out[16])
{
    if (r == NULL || !r->fixed || sb > 7U || out == NULL) {
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
    if (r == NULL || !r->fixed || sb > 7U || mask == NULL || cntl == NULL) {
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
