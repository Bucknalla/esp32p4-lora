#include "lwc/plan.h"

#include <string.h>

#include "lwc/region.h"

#define BAND_MIN_HZ 863000000U
#define BAND_MAX_HZ 870000000U

#define IF_MAX_125 400000 /* |IF| + 62.5 kHz <= 462.5 kHz: chan_ok for a 125 kHz channel */

static bool is_eu868(const lwc_region_t *r)
{
    return r->id == LWC_REGION_EU868;
}

static bool is_default(const lwc_region_t *r, uint32_t hz)
{
    for (size_t k = 0; k < r->n_default; k++) {
        if (r->default_ch[k].hz == hz) {
            return true;
        }
    }
    return false;
}

static void default_eu868(lwc_plan_t *p)
{
    p->radio_hz[0] = 867500000U;
    p->radio_hz[1] = 868500000U;
    static const lwc_plan_chan_t m[LWC_PLAN_MULTISF] = {
        {true, 1, -400000}, {true, 1, -200000}, {true, 1, 0},      {true, 0, -400000},
        {true, 0, -200000}, {true, 0, 0},       {true, 0, 200000}, {true, 0, 400000},
    };
    memcpy(p->multisf, m, sizeof m);
    p->std = (lwc_plan_chan_t){true, 1, -200000};
    p->std_sf = 7;
    p->std_bw_khz = 250;
    p->fsk = (lwc_plan_chan_t){false, 1, 300000};
}

bool lwc_plan_default(lwc_plan_t *p, const lwc_region_t *r)
{
    if (p == NULL) {
        return false;
    }
    memset(p, 0, sizeof *p);
    if (r == NULL) {
        return false;
    }
    if (r->fixed) {
        return lwc_plan_subband(p, r, lwc_plan_default_subband(r));
    }
    if (r->n_default == 0U) {
        return false;
    }
    if (is_eu868(r)) {
        default_eu868(p);
        return true;
    }
    /* The default channels on as few radios as fit (|IF| <= 400 kHz), lowest
     * first; radio 1 unused stays on radio 0's frequency. */
    size_t k = 0;
    for (uint8_t radio = 0; radio < 2U && k < r->n_default; radio++) {
        const uint32_t lo = r->default_ch[k].hz;
        size_t last = k;
        while (last + 1U < r->n_default && r->default_ch[last + 1U].hz - lo <= 2U * IF_MAX_125) {
            last++;
        }
        const uint32_t centre = lo + (r->default_ch[last].hz - lo) / 2U;
        p->radio_hz[radio] = centre;
        for (; k <= last; k++) {
            p->multisf[k] = (lwc_plan_chan_t){true, radio, (int32_t)r->default_ch[k].hz - (int32_t)centre};
        }
    }
    if (p->radio_hz[1] == 0U) {
        p->radio_hz[1] = p->radio_hz[0];
    }
    p->std_sf = 7;
    p->std_bw_khz = 125;
    return k == r->n_default;
}

uint8_t lwc_plan_default_subband(const lwc_region_t *r)
{
    if (r == NULL || !r->fixed) {
        return 0;
    }
    return r->id == LWC_REGION_CN470 ? 10U : 1U;
}

bool lwc_plan_subband(lwc_plan_t *p, const lwc_region_t *r, uint8_t sb)
{
    if (p == NULL) {
        return false;
    }
    memset(p, 0, sizeof *p);
    if (r == NULL || !r->fixed || sb >= lwc_region_subbands(r)) {
        return false;
    }
    /* 4 channels per radio. With a 500 kHz channel (it lies between channels
     * 3 and 4 of the sub-band), radio 0 sits on channel 2 so the 500 kHz
     * channel fits at IF +300 kHz; otherwise both radios are centred. */
    const uint32_t step = r->up125.step_hz;
    const uint32_t c0 = r->up125.base_hz + 8U * sb * step;
    const bool wide = r->n_up500 > 0U;
    p->radio_hz[0] = wide ? c0 + 2U * step : c0 + 3U * step / 2U;
    p->radio_hz[1] = c0 + 11U * step / 2U;
    for (uint8_t k = 0; k < LWC_PLAN_MULTISF; k++) {
        const uint8_t radio = k < 4U ? 0U : 1U;
        p->multisf[k] = (lwc_plan_chan_t){true, radio, (int32_t)(c0 + k * step) - (int32_t)p->radio_hz[radio]};
    }
    p->std_sf = 7;
    p->std_bw_khz = 125;
    if (wide) {
        const lwc_region_dr_t *d = &r->dr[r->up500.min_dr];
        const uint32_t hz = r->up500.base_hz + sb * r->up500.step_hz;
        p->std = (lwc_plan_chan_t){true, 0, (int32_t)hz - (int32_t)p->radio_hz[0]};
        p->std_sf = d->sf;
        p->std_bw_khz = d->bw_khz;
    }
    return true;
}

int lwc_plan_subband_of(const lwc_plan_t *p, const lwc_region_t *r)
{
    if (p == NULL || r == NULL || !r->fixed) {
        return -1;
    }
    for (uint8_t sb = 0; sb < lwc_region_subbands(r); sb++) {
        size_t found = 0;
        for (size_t k = 0; k < 8U; k++) {
            const uint32_t hz = r->up125.base_hz + (8U * sb + (uint32_t)k) * r->up125.step_hz;
            for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
                if (lwc_plan_hz(p, &p->multisf[i]) == hz) {
                    found++;
                    break;
                }
            }
        }
        if (found == 8U) {
            return sb;
        }
    }
    return -1;
}

uint32_t lwc_plan_hz(const lwc_plan_t *p, const lwc_plan_chan_t *c)
{
    if (p == NULL || c == NULL || !c->enable || c->radio > 1U) {
        return 0;
    }
    return (uint32_t)((int64_t)p->radio_hz[c->radio] + c->if_hz);
}

/* SX1301 HAL (loragw_hal.c lgw_rxif_setconf): the channel must fit in the
 * radio's receive band, which depends on the channel bandwidth. */
static bool chan_ok(const lwc_plan_t *p, const lwc_plan_chan_t *c, bool band, uint32_t bw_hz, uint32_t rf_bw_hz,
                    lwc_plan_err_t *e)
{
    if (!c->enable) {
        return true;
    }
    const int64_t a = c->if_hz < 0 ? -(int64_t)c->if_hz : c->if_hz;
    if (c->radio > 1U || a + (int64_t)bw_hz / 2 > (int64_t)rf_bw_hz / 2) {
        *e = LWC_PLAN_ERR_IF;
        return false;
    }
    const uint32_t hz = lwc_plan_hz(p, c);
    if (band && (hz - bw_hz / 2U < BAND_MIN_HZ || hz + bw_hz / 2U > BAND_MAX_HZ)) {
        *e = LWC_PLAN_ERR_BAND;
        return false;
    }
    return true;
}

/* fixed regions: every channel on the uplink grid, std at an uplink DR */
static lwc_plan_err_t fixed_ok(const lwc_plan_t *p, const lwc_region_t *rg)
{
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        if (p->multisf[i].enable && lwc_region_channel(rg, lwc_plan_hz(p, &p->multisf[i]), 125) < 0) {
            return LWC_PLAN_ERR_CHANNEL;
        }
    }
    if (p->std.enable) {
        if (p->std_bw_khz != 500U || lwc_region_channel(rg, lwc_plan_hz(p, &p->std), 500) < 0) {
            return LWC_PLAN_ERR_CHANNEL;
        }
        if (lwc_region_dr_of(rg, p->std_sf, 500, false) < 0) {
            return LWC_PLAN_ERR_STD;
        }
    }
    return p->fsk.enable ? LWC_PLAN_ERR_CHANNEL : LWC_PLAN_OK;
}

static bool card_fits(const lwc_plan_t *p, const lwc_plan_chan_t *ch, uint32_t bw_hz, const lwc_card_t *c)
{
    return !ch->enable || lwc_card_fits(c, lwc_plan_hz(p, ch), bw_hz);
}

lwc_plan_err_t lwc_plan_check(const lwc_plan_t *p, const lwc_region_t *rg, const lwc_card_t *card)
{
    if (p == NULL || rg == NULL || card == NULL) {
        return LWC_PLAN_ERR_ARG;
    }
    if (!lwc_card_region_ok(card, rg)) {
        return LWC_PLAN_ERR_REGION;
    }
    const bool band = is_eu868(rg);
    for (size_t r = 0; r < 2U; r++) {
        if (band && (p->radio_hz[r] < BAND_MIN_HZ || p->radio_hz[r] > BAND_MAX_HZ)) {
            return LWC_PLAN_ERR_RADIO;
        }
    }
    lwc_plan_err_t e = LWC_PLAN_OK;
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        if (!chan_ok(p, &p->multisf[i], band, 125000U, 925000U, &e)) {
            return e;
        }
    }
    if (p->std.enable) {
        if (p->std_sf < 7U || p->std_sf > 12U ||
            (p->std_bw_khz != 125U && p->std_bw_khz != 250U && p->std_bw_khz != 500U)) {
            return LWC_PLAN_ERR_STD;
        }
        const uint32_t rf = p->std_bw_khz == 125U ? 925000U : p->std_bw_khz == 250U ? 1000000U : 1100000U;
        if (!chan_ok(p, &p->std, band, (uint32_t)p->std_bw_khz * 1000U, rf, &e)) {
            return e;
        }
    }
    if (!chan_ok(p, &p->fsk, band, 125000U, 925000U, &e)) { /* HAL: FSK at its 125 kHz default */
        return e;
    }
    if (rg->fixed && (e = fixed_ok(p, rg)) != LWC_PLAN_OK) {
        return e;
    }
    for (size_t d = 0; d < rg->n_default; d++) {
        bool found = false;
        for (size_t i = 0; i < LWC_PLAN_MULTISF && !found; i++) {
            found = lwc_plan_hz(p, &p->multisf[i]) == rg->default_ch[d].hz;
        }
        if (!found) {
            return LWC_PLAN_ERR_DEFAULT;
        }
    }
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        if (!card_fits(p, &p->multisf[i], 125000U, card)) {
            return LWC_PLAN_ERR_CARD;
        }
    }
    if (!card_fits(p, &p->std, (uint32_t)p->std_bw_khz * 1000U, card) || !card_fits(p, &p->fsk, 125000U, card)) {
        return LWC_PLAN_ERR_CARD;
    }
    return LWC_PLAN_OK;
}

size_t lwc_plan_cflist(const lwc_plan_t *p, const lwc_region_t *r, uint32_t out[5])
{
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, 5U * sizeof out[0]);
    if (p == NULL || r == NULL || r->fixed) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        const uint32_t hz = lwc_plan_hz(p, &p->multisf[i]);
        bool skip = hz == 0U || is_default(r, hz);
        for (size_t j = 0; j < n && !skip; j++) {
            skip = out[j] == hz;
        }
        if (skip) {
            continue;
        }
        if (n < 5U) {
            out[n++] = hz;
        } else if (hz < out[4]) {
            out[4] = hz; /* keep the five lowest */
        } else {
            continue;
        }
        for (size_t j = n - 1U; j > 0U && out[j] < out[j - 1U]; j--) { /* insertion sort */
            const uint32_t t = out[j];
            out[j] = out[j - 1U];
            out[j - 1U] = t;
        }
    }
    return n;
}

bool lwc_plan_equal(const lwc_plan_t *a, const lwc_plan_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->radio_hz[0] != b->radio_hz[0] || a->radio_hz[1] != b->radio_hz[1] || a->std_sf != b->std_sf ||
        a->std_bw_khz != b->std_bw_khz) {
        return false;
    }
    const lwc_plan_chan_t *ca[LWC_PLAN_MULTISF + 2U], *cb[LWC_PLAN_MULTISF + 2U];
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        ca[i] = &a->multisf[i];
        cb[i] = &b->multisf[i];
    }
    ca[LWC_PLAN_MULTISF] = &a->std;
    cb[LWC_PLAN_MULTISF] = &b->std;
    ca[LWC_PLAN_MULTISF + 1U] = &a->fsk;
    cb[LWC_PLAN_MULTISF + 1U] = &b->fsk;
    for (size_t i = 0; i < LWC_PLAN_MULTISF + 2U; i++) {
        if (ca[i]->enable != cb[i]->enable ||
            (ca[i]->enable && (ca[i]->radio != cb[i]->radio || ca[i]->if_hz != cb[i]->if_hz))) {
            return false;
        }
    }
    return true;
}

static void put32(uint8_t **o, uint32_t v)
{
    for (unsigned i = 0; i < 4U; i++) {
        *(*o)++ = (uint8_t)(v >> (8U * i));
    }
}

static uint32_t get32(const uint8_t **in)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4U; i++) {
        v |= (uint32_t)*(*in)++ << (8U * i);
    }
    return v;
}

static void put_chan(uint8_t **o, const lwc_plan_chan_t *c)
{
    *(*o)++ = c->enable ? 1U : 0U;
    *(*o)++ = c->radio;
    put32(o, (uint32_t)c->if_hz);
}

static bool get_chan(const uint8_t **in, lwc_plan_chan_t *c)
{
    const uint8_t en = *(*in)++;
    c->enable = en == 1U;
    c->radio = *(*in)++;
    c->if_hz = (int32_t)get32(in);
    return en <= 1U;
}

/* 1 version + 2 radios x 4 + 10 channels (8 multi-SF, std, FSK) x 6 + std SF 1
 * + std bandwidth 2 = 72 bytes. */
bool lwc_plan_pack(const lwc_plan_t *p, uint8_t *out, size_t cap)
{
    if (p == NULL || out == NULL || cap < LWC_PLAN_PACKED) {
        return false;
    }
    uint8_t *o = out;
    *o++ = LWC_PLAN_VERSION;
    put32(&o, p->radio_hz[0]);
    put32(&o, p->radio_hz[1]);
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        put_chan(&o, &p->multisf[i]);
    }
    put_chan(&o, &p->std);
    *o++ = p->std_sf;
    *o++ = (uint8_t)p->std_bw_khz;
    *o++ = (uint8_t)(p->std_bw_khz >> 8);
    put_chan(&o, &p->fsk);
    return (size_t)(o - out) == LWC_PLAN_PACKED;
}

bool lwc_plan_unpack(const uint8_t *in, size_t n, const lwc_region_t *r, const lwc_card_t *card, lwc_plan_t *p)
{
    if (in == NULL || p == NULL || n != LWC_PLAN_PACKED || in[0] != LWC_PLAN_VERSION) {
        return false;
    }
    lwc_plan_t t;
    memset(&t, 0, sizeof t);
    const uint8_t *i = &in[1];
    t.radio_hz[0] = get32(&i);
    t.radio_hz[1] = get32(&i);
    bool ok = true;
    for (size_t c = 0; c < LWC_PLAN_MULTISF; c++) {
        ok = get_chan(&i, &t.multisf[c]) && ok;
    }
    ok = get_chan(&i, &t.std) && ok;
    t.std_sf = *i++;
    t.std_bw_khz = (uint16_t)(i[0] | (uint16_t)(i[1] << 8));
    i += 2;
    ok = get_chan(&i, &t.fsk) && ok;
    if (!ok || lwc_plan_check(&t, r, card) != LWC_PLAN_OK) {
        return false;
    }
    *p = t;
    return true;
}

