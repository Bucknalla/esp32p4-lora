#include "lwc/txrule.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define T(id) lwc_txrule(id)

/* EU868, the one region that transmits: the frequencies the firmware uses are
 * accepted at the same power cap as before R4 (16 dBm; 27 dBm in g3). The
 * band limits themselves are pinned by the golden snapshot. */
static void eu868_pinned(void)
{
    const lwc_txrule_t *eu = T(LWC_REGION_EU868);
    static const uint32_t ok16[] = {868100000U, 868300000U, 868500000U, 867100000U, 867300000U,
                                    867500000U, 867700000U, 867900000U, 868800000U, 869800000U};
    for (size_t i = 0; i < sizeof ok16 / sizeof ok16[0]; i++) {
        int8_t m = 0;
        CHECK(lwc_txrule_check(eu, ok16[i], false, &m) == LWC_TXR_OK && m == 16);
    }
    int8_t m = 0;
    CHECK(lwc_txrule_check(eu, 869525000U, false, &m) == LWC_TXR_OK && m == 27); /* RX2 */
    CHECK(lwc_txrule_check(eu, 863000000U, false, NULL) == LWC_TXR_OK);
    CHECK(lwc_txrule_check(eu, 862999999U, false, NULL) == LWC_TXR_FREQ);
    CHECK(lwc_txrule_check(eu, 868650000U, false, NULL) == LWC_TXR_FREQ); /* g1-g2 gap */
    CHECK(lwc_txrule_check(eu, 869300000U, false, NULL) == LWC_TXR_FREQ); /* g2-g3 gap */
    CHECK(lwc_txrule_check(eu, 870000000U, false, NULL) == LWC_TXR_FREQ);
    CHECK(!eu->lbt && eu->chan_dc_permille == 0U);
}

/* The rules never open the gate: only EU868 transmits, and it has a full row. */
static void gate(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
        const lwc_txrule_t *t = T((lwc_region_id_t)i);
        CHECK(r != NULL && t != NULL && t->id == (lwc_region_id_t)i);
        CHECK(lwc_region_tx_enabled(r) == (i == LWC_REGION_EU868));
        if (lwc_region_tx_enabled(r)) {
            CHECK(t->n_bands > 0U && !t->lbt);
            CHECK(lwc_txrule_check(t, r->rx2_hz, false, NULL) == LWC_TXR_OK);
        }
    }
    CHECK(T(LWC_REGIONS) == NULL);
    CHECK(lwc_txrule_check(NULL, 868100000U, true, NULL) == LWC_TXR_NO_RULES);
}

/* Every row with a band list holds the region's own downlink frequencies. */
static void rows_hold_region(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
        const lwc_txrule_t *t = T((lwc_region_id_t)i);
        if (t->n_bands == 0U) {
            continue;
        }
        CHECK(t->n_bands <= LWC_TXRULE_BANDS);
        for (size_t b = 0; b < t->n_bands; b++) {
            CHECK(t->band[b].lo_hz < t->band[b].hi_hz && t->band[b].max_dbm > 0 && t->band[b].max_dbm <= 30);
            CHECK(t->band[b].dc_permille <= 100U);
        }
        CHECK(lwc_txrule_band(t, r->rx2_hz) >= 0);
        if (r->fixed) {
            for (uint8_t c = 0; c < r->n_down; c++) {
                CHECK(lwc_txrule_band(t, r->down.base_hz + (uint32_t)c * r->down.step_hz) >= 0);
            }
        } else {
            for (uint8_t c = 0; c < r->n_default; c++) {
                CHECK(lwc_txrule_band(t, r->default_ch[c].hz) >= 0);
            }
        }
    }
}

static void unsourced_and_lbt(void)
{
    /* no sourced band list: never OK */
    static const lwc_region_id_t none[] = {LWC_REGION_AS923_1, LWC_REGION_AS923_2, LWC_REGION_AS923_3,
                                           LWC_REGION_AS923_4, LWC_REGION_CN470};
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        const lwc_region_t *r = lwc_region(none[i]);
        CHECK(lwc_txrule_check(T(none[i]), r->rx2_hz, true, NULL) == LWC_TXR_NO_RULES);
    }
    /* AS923-1: LBT and per-channel duty are known, the band is not */
    CHECK(T(LWC_REGION_AS923_1)->lbt && T(LWC_REGION_AS923_1)->lbt_dbm == -80 &&
          T(LWC_REGION_AS923_1)->lbt_us == 5000U && T(LWC_REGION_AS923_1)->chan_dc_permille == 100U);
    /* KR920 needs LBT: refused without it, accepted with it */
    const lwc_txrule_t *kr = T(LWC_REGION_KR920);
    int8_t m = 0;
    CHECK(lwc_txrule_check(kr, 922100000U, false, &m) == LWC_TXR_LBT);
    CHECK(lwc_txrule_check(kr, 922100000U, true, &m) == LWC_TXR_OK && m == 14);
    CHECK(lwc_txrule_check(kr, 921900000U, true, &m) == LWC_TXR_OK && m == 10); /* RX2: below 922.1 MHz */
    CHECK(lwc_txrule_check(kr, 920900000U, true, &m) == LWC_TXR_OK && m == 10);
    CHECK(lwc_txrule_check(kr, 923500000U, true, NULL) == LWC_TXR_FREQ);
    CHECK(kr->lbt_dbm == -67 && kr->lbt_us == 6000U);
    /* the others: no LBT, a band list */
    CHECK(lwc_txrule_check(T(LWC_REGION_US915), 923300000U, false, &m) == LWC_TXR_OK && m == 26);
    CHECK(lwc_txrule_check(T(LWC_REGION_US915), 927500000U, false, NULL) == LWC_TXR_OK);
    CHECK(lwc_txrule_check(T(LWC_REGION_US915), 927600000U, false, NULL) == LWC_TXR_FREQ);
    CHECK(lwc_txrule_check(T(LWC_REGION_AU915), 923300000U, false, &m) == LWC_TXR_OK && m == 30);
    CHECK(lwc_txrule_check(T(LWC_REGION_IN865), 866550000U, false, &m) == LWC_TXR_OK && m == 30);
    CHECK(lwc_txrule_check(T(LWC_REGION_RU864), 869100000U, false, &m) == LWC_TXR_OK && m == 16);
    CHECK(lwc_txrule_check(T(LWC_REGION_EU433), 434665000U, false, &m) == LWC_TXR_OK && m == 12);
    CHECK(lwc_txrule_check(T(LWC_REGION_EU433), 434665001U, false, NULL) == LWC_TXR_FREQ);
    CHECK(lwc_txr_str(LWC_TXR_LBT)[0] == 'L');
}

static void dutycycle(void)
{
    const lwc_txrule_t *eu = T(LWC_REGION_EU868);
    lwc_dutycycle_t dc;
    lwc_dc_init(&dc, eu, 0);
    CHECK(lwc_txrule_band(eu, 868100000U) == 2 && lwc_dc_permille(eu, 2) == 10);
    CHECK(lwc_txrule_band(eu, 869525000U) == 4 && lwc_dc_permille(eu, 4) == 100);
    CHECK(lwc_dc_permille(eu, -1) == 0U && lwc_dc_permille(eu, 6) == 0U);
    /* g1 1 %: 36 s per hour */
    CHECK(lwc_dc_take(&dc, eu, 0, 868100000U, 36000000U));
    CHECK(!lwc_dc_take(&dc, eu, 0, 868100000U, 1U));
    /* 100 s later: 1 s refilled */
    CHECK(lwc_dc_take(&dc, eu, 100000000LL, 868100000U, 1000000U));
    CHECK(!lwc_dc_take(&dc, eu, 100000000LL, 868100000U, 1U));
    /* other band unaffected */
    CHECK(lwc_dc_take(&dc, eu, 100000000LL, 869525000U, 1000000U));
    CHECK(!lwc_dc_take(&dc, eu, 0, 870500000U, 1U));

    /* no duty cycle: any airtime */
    const lwc_txrule_t *us = T(LWC_REGION_US915);
    lwc_dc_init(&dc, us, 0);
    CHECK(lwc_dc_take(&dc, us, 0, 923300000U, 3600000000U));
    CHECK(!lwc_dc_take(&dc, us, 0, 902000000U, 1U));

    /* KR920: 2 % per channel, 72 s per hour each */
    const lwc_txrule_t *kr = T(LWC_REGION_KR920);
    lwc_dc_init(&dc, kr, 0);
    CHECK(lwc_dc_take(&dc, kr, 0, 922100000U, 72000000U));
    CHECK(!lwc_dc_take(&dc, kr, 0, 922100000U, 1U));
    CHECK(lwc_dc_take(&dc, kr, 0, 922300000U, 72000000U)); /* another channel */
    CHECK(lwc_dc_take(&dc, kr, 50000000LL, 922100000U, 1000000U)); /* 50 s: 1 s back */
    CHECK(!lwc_dc_take(&dc, kr, 50000000LL, 922100000U, 1U));
    /* the channel list is full after 16 */
    lwc_dc_init(&dc, kr, 0);
    for (uint32_t c = 0; c < LWC_DC_CHANNELS; c++) {
        CHECK(lwc_dc_take(&dc, kr, 0, 920900000U + c * 100000U, 1U));
    }
    CHECK(!lwc_dc_take(&dc, kr, 0, 920900000U + LWC_DC_CHANNELS * 100000U, 1U));

    /* booted in another region, then EU868, nothing sent yet: full budgets,
     * as before R4 */
    const lwc_txrule_t *ru = T(LWC_REGION_RU864);
    lwc_dc_init(&dc, ru, 0);
    CHECK(lwc_dc_take(&dc, eu, 0, 868100000U, 36000000U));
    CHECK(!lwc_dc_take(&dc, eu, 0, 868100000U, 1U));
    lwc_dc_init(&dc, T(LWC_REGION_AS923_1), 0);
    CHECK(lwc_dc_take(&dc, eu, 0, 869525000U, 360000000U)); /* g3 10 %: 360 s */
    /* after a frame, a region change starts from empty budgets: it cannot refill them */
    lwc_dc_init(&dc, eu, 0);
    CHECK(lwc_dc_take(&dc, eu, 0, 868100000U, 1U));
    CHECK(!lwc_dc_take(&dc, kr, 0, 922100000U, 1U));
    CHECK(lwc_dc_take(&dc, kr, 50000000LL, 922100000U, 1000000U)); /* refills at 2 % */
    CHECK(!lwc_dc_take(&dc, eu, 0, 868100000U, 1U));
    CHECK(lwc_dc_take(&dc, eu, 100000000LL, 868100000U, 1000000U)); /* 100 s at 1 % */
    CHECK(!lwc_dc_take(&dc, eu, 100000000LL, 868100000U, 1U));
    CHECK(!lwc_dc_take(NULL, eu, 0, 868100000U, 1U) && !lwc_dc_take(&dc, NULL, 0, 868100000U, 1U));
}

int main(void)
{
    eu868_pinned();
    gate();
    rows_hold_region();
    unsourced_and_lbt();
    dutycycle();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("txrule: ok");
    return 0;
}
