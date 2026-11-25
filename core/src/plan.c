#include "lwc/plan.h"

#include <string.h>

#include "lwc/region.h"

#define BAND_MIN_HZ 863000000U
#define BAND_MAX_HZ 870000000U

/* EU868 default channel k (RP002): the region table. */
static uint32_t k_default(size_t k)
{
    return lwc_region(LWC_REGION_EU868)->default_ch[k].hz;
}

void lwc_plan_default_eu868(lwc_plan_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof *p);
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

uint32_t lwc_plan_hz(const lwc_plan_t *p, const lwc_plan_chan_t *c)
{
    if (p == NULL || c == NULL || !c->enable || c->radio > 1U) {
        return 0;
    }
    return (uint32_t)((int64_t)p->radio_hz[c->radio] + c->if_hz);
}

/* SX1301 HAL (loragw_hal.c lgw_rxif_setconf): the channel must fit in the
 * radio's receive band, which depends on the channel bandwidth. */
static bool chan_ok(const lwc_plan_t *p, const lwc_plan_chan_t *c, uint32_t bw_hz, uint32_t rf_bw_hz,
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
    if (hz - bw_hz / 2U < BAND_MIN_HZ || hz + bw_hz / 2U > BAND_MAX_HZ) {
        *e = LWC_PLAN_ERR_BAND;
        return false;
    }
    return true;
}

lwc_plan_err_t lwc_plan_check(const lwc_plan_t *p)
{
    if (p == NULL) {
        return LWC_PLAN_ERR_ARG;
    }
    for (size_t r = 0; r < 2U; r++) {
        if (p->radio_hz[r] < BAND_MIN_HZ || p->radio_hz[r] > BAND_MAX_HZ) {
            return LWC_PLAN_ERR_RADIO;
        }
    }
    lwc_plan_err_t e = LWC_PLAN_OK;
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        if (!chan_ok(p, &p->multisf[i], 125000U, 925000U, &e)) {
            return e;
        }
    }
    if (p->std.enable) {
        if (p->std_sf < 7U || p->std_sf > 12U ||
            (p->std_bw_khz != 125U && p->std_bw_khz != 250U && p->std_bw_khz != 500U)) {
            return LWC_PLAN_ERR_STD;
        }
        const uint32_t rf = p->std_bw_khz == 125U ? 925000U : p->std_bw_khz == 250U ? 1000000U : 1100000U;
        if (!chan_ok(p, &p->std, (uint32_t)p->std_bw_khz * 1000U, rf, &e)) {
            return e;
        }
    }
    if (!chan_ok(p, &p->fsk, 125000U, 925000U, &e)) { /* HAL: FSK at its 125 kHz default */
        return e;
    }
    for (size_t d = 0; d < 3U; d++) {
        bool found = false;
        for (size_t i = 0; i < LWC_PLAN_MULTISF && !found; i++) {
            found = lwc_plan_hz(p, &p->multisf[i]) == k_default(d);
        }
        if (!found) {
            return LWC_PLAN_ERR_DEFAULT;
        }
    }
    return LWC_PLAN_OK;
}

size_t lwc_plan_cflist(const lwc_plan_t *p, uint32_t out[5])
{
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, 5U * sizeof out[0]);
    if (p == NULL) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < LWC_PLAN_MULTISF; i++) {
        const uint32_t hz = lwc_plan_hz(p, &p->multisf[i]);
        bool skip = hz == 0U;
        for (size_t d = 0; d < 3U && !skip; d++) {
            skip = hz == k_default(d);
        }
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

bool lwc_plan_unpack(const uint8_t *in, size_t n, lwc_plan_t *p)
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
    if (!ok || lwc_plan_check(&t) != LWC_PLAN_OK) {
        return false;
    }
    *p = t;
    return true;
}

