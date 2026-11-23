#include "lwc/txpk.h"

#include <string.h>

/* ==== airtime ============================================================ */

uint32_t lwc_lora_airtime_us(uint8_t sf, uint16_t bw_khz, uint8_t cr_denom, uint16_t preamble,
                             uint16_t size, bool crc, bool implicit_header)
{
    if (sf < 6U || sf > 12U || (bw_khz != 125U && bw_khz != 250U && bw_khz != 500U) ||
        cr_denom < 5U || cr_denom > 8U || size > 255U) {
        return 0;
    }
    /* Symbol time in ns: 2^SF / BW. Exact for these bandwidths. */
    const uint64_t tsym_ns = ((uint64_t)1U << sf) * 1000000U / bw_khz;
    const int de = tsym_ns >= 16000000U ? 1 : 0;
    const int64_t bits = 8 * (int64_t)size - 4 * (int64_t)sf + 28 + (crc ? 16 : 0) -
                        (implicit_header ? 20 : 0);
    const int64_t den = 4 * ((int64_t)sf - 2 * de);
    int64_t blocks = 0;
    if (bits > 0) {
        blocks = (bits + den - 1) / den;
    }
    const uint64_t payload_sym = 8U + (uint64_t)blocks * cr_denom;
    /* Preamble: (n + 4.25) symbols = (4n + 17) / 4 symbols. */
    const uint64_t total_ns =
        ((4U * (uint64_t)preamble + 17U) * tsym_ns) / 4U + payload_sym * tsym_ns;
    return (uint32_t)((total_ns + 999U) / 1000U);
}

/* ==== JIT queue ========================================================== */

static int32_t diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

void lwc_jit_init(lwc_jit_t *q, uint32_t min_lead_us, uint32_t max_advance_us, uint32_t guard_us)
{
    memset(q, 0, sizeof(*q));
    q->min_lead_us = min_lead_us;
    q->max_advance_us = max_advance_us;
    q->guard_us = guard_us;
}

lwc_jit_res_t lwc_jit_admit(lwc_jit_t *q, uint32_t now, uint32_t start, uint32_t airtime_us,
                            uint32_t id, size_t *slot)
{
    const int32_t lead = diff(start, now);
    if (lead < (int32_t)q->min_lead_us) {
        return LWC_JIT_TOO_LATE;
    }
    if ((uint32_t)lead > q->max_advance_us) {
        return LWC_JIT_TOO_EARLY;
    }
    size_t free_slot = LWC_JIT_SLOTS;
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        const lwc_jit_slot_t *s = &q->slot[i];
        if (!s->used) {
            if (free_slot == LWC_JIT_SLOTS) {
                free_slot = i;
            }
            continue;
        }
        /* Overlap if neither frame ends (plus guard) before the other starts. */
        const int32_t a = diff(start, s->start);
        const int64_t new_end = (int64_t)a + airtime_us + q->guard_us;
        const int64_t old_end = (int64_t)s->airtime_us + q->guard_us;
        if (new_end > 0 && (int64_t)a < old_end) {
            return LWC_JIT_COLLISION;
        }
    }
    if (free_slot == LWC_JIT_SLOTS) {
        return LWC_JIT_FULL;
    }
    q->slot[free_slot] = (lwc_jit_slot_t){.used = true, .start = start, .airtime_us = airtime_us, .id = id};
    if (slot != NULL) {
        *slot = free_slot;
    }
    return LWC_JIT_OK;
}

int lwc_jit_due(const lwc_jit_t *q, uint32_t now, uint32_t lead_us, uint32_t min_send_us,
                int *expired)
{
    int best = -1;
    int32_t best_d = 0;
    if (expired != NULL) {
        *expired = -1;
    }
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        if (!q->slot[i].used || q->slot[i].sent) {
            continue;
        }
        const int32_t d = diff(q->slot[i].start, now);
        if (d < (int32_t)min_send_us) {
            if (expired != NULL && *expired < 0) {
                *expired = (int)i;
            }
            continue;
        }
        if (d <= (int32_t)lead_us && (best < 0 || d < best_d)) {
            best = (int)i;
            best_d = d;
        }
    }
    return best;
}

void lwc_jit_sent(lwc_jit_t *q, size_t slot)
{
    if (slot < LWC_JIT_SLOTS) {
        q->slot[slot].sent = true;
    }
}

int lwc_jit_ended(const lwc_jit_t *q, uint32_t now, uint32_t margin_us)
{
    for (size_t i = 0; i < LWC_JIT_SLOTS; i++) {
        const lwc_jit_slot_t *s = &q->slot[i];
        if (s->used && s->sent &&
            (int64_t)diff(now, s->start) > (int64_t)s->airtime_us + margin_us) {
            return (int)i;
        }
    }
    return -1;
}

void lwc_jit_release(lwc_jit_t *q, size_t slot)
{
    if (slot < LWC_JIT_SLOTS) {
        q->slot[slot].used = false;
    }
}

/* ==== duty cycle ========================================================= */

static const struct {
    uint32_t lo, hi; /* Hz, [lo, hi) */
    uint32_t permille;
} k_bands[LWC_DC_BANDS] = {
    {863000000U, 865000000U, 1U},   /* 0.1 % */
    {865000000U, 868000000U, 10U},  /* 1 % */
    {868000000U, 868600000U, 10U},  /* g1: 1 % */
    {868700000U, 869200000U, 1U},   /* g2: 0.1 % */
    {869400000U, 869650000U, 100U}, /* g3: 10 % (RX2 869.525) */
    {869700000U, 870000000U, 10U},  /* g4: 1 % */
};

#define DC_WINDOW_US 3600000000LL

static int64_t capacity(size_t b)
{
    return DC_WINDOW_US * (int64_t)k_bands[b].permille / 1000;
}

void lwc_dc_init(lwc_dutycycle_t *dc, int64_t now_us)
{
    dc->last_us = now_us;
    for (size_t b = 0; b < LWC_DC_BANDS; b++) {
        dc->tokens_us[b] = capacity(b);
    }
}

int lwc_dc_band(uint32_t freq_hz)
{
    for (size_t b = 0; b < LWC_DC_BANDS; b++) {
        if (freq_hz >= k_bands[b].lo && freq_hz < k_bands[b].hi) {
            return (int)b;
        }
    }
    return -1;
}

uint32_t lwc_dc_permille(int band)
{
    return (band >= 0 && (size_t)band < LWC_DC_BANDS) ? k_bands[band].permille : 0U;
}

bool lwc_dc_take(lwc_dutycycle_t *dc, int64_t now_us, uint32_t freq_hz, uint32_t airtime_us)
{
    const int b = lwc_dc_band(freq_hz);
    if (b < 0) {
        return false;
    }
    const int64_t elapsed = now_us - dc->last_us;
    if (elapsed > 0) {
        for (size_t i = 0; i < LWC_DC_BANDS; i++) {
            const int64_t refill = elapsed * (int64_t)k_bands[i].permille / 1000;
            const int64_t t = dc->tokens_us[i] + refill;
            dc->tokens_us[i] = t > capacity(i) ? capacity(i) : t;
        }
        dc->last_us = now_us;
    }
    if (dc->tokens_us[b] < (int64_t)airtime_us) {
        return false;
    }
    dc->tokens_us[b] -= (int64_t)airtime_us;
    return true;
}
