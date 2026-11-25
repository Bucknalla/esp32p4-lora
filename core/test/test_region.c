#include "lwc/region.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static const lwc_region_t *R(lwc_region_id_t id)
{
    return lwc_region(id);
}

/* RP002 values typed by hand, independent of the generator and its reference. */
static void spot_values(void)
{
    const lwc_region_t *eu = R(LWC_REGION_EU868), *us = R(LWC_REGION_US915), *au = R(LWC_REGION_AU915);
    CHECK(eu && strcmp(eu->name, "EU868") == 0 && eu->rx2_hz == 869525000U && eu->rx2_dr == 0 && eu->max_eirp_dbm == 16);
    CHECK(eu->dr[0].sf == 12 && eu->dr[0].bw_khz == 125 && lwc_region_max_n(eu, 0, false) == 51U);
    CHECK(lwc_region_max_n(eu, 3, false) == 115U && lwc_region_max_n(eu, 5, false) == 222U);
    CHECK(eu->dr[6].sf == 7 && eu->dr[6].bw_khz == 250 && eu->dr[7].kind == LWC_DR_FSK);
    CHECK(eu->n_default == 3 && eu->default_ch[0].hz == 868100000U && eu->default_ch[2].hz == 868500000U);
    CHECK(lwc_region_rx1_dr(eu, 5, 2, false) == 3 && lwc_region_rx1_dr(eu, 0, 3, false) == 0);
    CHECK(lwc_region_rx1_dr(eu, 0, 0, true) == 0); /* no dwell floor in EU868 */
    /* US915: 64 + 8 channels, RX1 on the 500 kHz downlink channels */
    CHECK(us->fixed && us->rx2_hz == 923300000U && us->rx2_dr == 8 && us->max_eirp_dbm == 30);
    CHECK(us->dr[0].sf == 10 && us->dr[0].bw_khz == 125 && lwc_region_max_n(us, 0, false) == 11U);
    CHECK(us->dr[4].sf == 8 && us->dr[4].bw_khz == 500 && us->dr[8].sf == 12 && us->dr[8].bw_khz == 500);
    CHECK(us->dr[7].kind == LWC_DR_NONE && lwc_region_max_n(us, 7, false) == 0U);
    CHECK(lwc_region_rx1_dr(us, 0, 0, false) == 10 && lwc_region_rx1_dr(us, 4, 0, false) == 13 && lwc_region_rx1_dr(us, 0, 4, false) == -1);
    CHECK(lwc_region_channel(us, 902300000U, 125) == 0 && lwc_region_channel(us, 903900000U, 125) == 8);
    CHECK(lwc_region_channel(us, 914900000U, 125) == 63 && lwc_region_channel(us, 915100000U, 125) == -1);
    CHECK(lwc_region_channel(us, 903000000U, 500) == 64 && lwc_region_channel(us, 914200000U, 500) == 71);
    CHECK(lwc_region_channel(us, 903000000U, 125) == -1 && lwc_region_channel(us, 902400000U, 125) == -1);
    CHECK(lwc_region_rx1_hz(us, 903900000U, 125) == 923300000U && lwc_region_rx1_hz(us, 904100000U, 125) == 923900000U);
    CHECK(lwc_region_rx1_hz(us, 904600000U, 500) == 923900000U && lwc_region_rx1_hz(us, 1U, 125) == 0U);
    /* AU915: the same rule at other frequencies */
    CHECK(au->fixed && au->up125.base_hz == 915200000U && au->up500.base_hz == 915900000U && au->rx2_hz == 923300000U);
    CHECK(lwc_region_rx1_hz(au, 916800000U, 125) == 923300000U); /* channel 8 */
    /* dynamic plans */
    const lwc_region_t *as1 = R(LWC_REGION_AS923_1), *as2 = R(LWC_REGION_AS923_2), *kr = R(LWC_REGION_KR920);
    const lwc_region_t *in = R(LWC_REGION_IN865), *ru = R(LWC_REGION_RU864);
    CHECK(strcmp(as1->name, "AS923-1") == 0 && as1->default_ch[0].hz == 923200000U && as1->rx2_dr == 2);
    CHECK(lwc_region_rx1_dr(as1, 0, 0, false) == 0 && lwc_region_rx1_dr(as1, 0, 0, true) == 2); /* dwell: DR2 floor */
    CHECK(lwc_region_rx1_dr(as1, 5, 1, true) == 4 && lwc_region_rx1_dr(as1, 7, 0, true) == lwc_region_rx1_dr(as1, 7, 0, false));
    CHECK(as1->tx_param_setup && lwc_region_max_n(as1, 2, true) < lwc_region_max_n(as1, 2, false));
    CHECK(as2->default_ch[0].hz == 921400000U && as2->rx2_hz == 921400000U);
    CHECK(kr->max_eirp_dbm == 14 && kr->rx2_hz == 921900000U && kr->n_default == 3);
    CHECK(in->rx2_hz == 866550000U && in->rx2_dr == 2 && in->max_eirp_dbm == 30);
    CHECK(ru->rx2_hz == 869100000U && ru->n_default == 2);
    CHECK(lwc_region_rx1_hz(eu, 868300000U, 125) == 868300000U && lwc_region_channel(eu, 868300000U, 125) == -1);
    /* TX power */
    CHECK(lwc_region_tx_power_dbm(eu, 0) == 16 && lwc_region_tx_power_dbm(eu, 7) == 2 && lwc_region_tx_power_dbm(eu, 8) == INT8_MIN);
    CHECK(lwc_region_tx_power_dbm(us, 10) == 10 && lwc_region_tx_power_dbm(NULL, 0) == INT8_MIN);
}

static void lookup(void)
{
    lwc_region_id_t id;
    CHECK(lwc_region_parse("EU868", &id) && id == LWC_REGION_EU868);
    CHECK(lwc_region_parse("AS923-4", &id) && id == LWC_REGION_AS923_4);
    CHECK(!lwc_region_parse("eu868", &id) && !lwc_region_parse("AS923", &id) && !lwc_region_parse(NULL, &id));
    CHECK(lwc_region((lwc_region_id_t)LWC_REGIONS) == NULL);
    const lwc_region_t *eu = R(LWC_REGION_EU868), *us = R(LWC_REGION_US915);
    CHECK(lwc_region_dr_of(eu, 7, 250, false) == 6 && lwc_region_dr_of(eu, 7, 500, false) == -1);
    CHECK(lwc_region_dr_of(eu, 12, 125, true) == 0); /* dynamic plans: one table */
    CHECK(lwc_region_dr_of(us, 8, 500, false) == 4 && lwc_region_dr_of(us, 8, 500, true) == 12);
    CHECK(lwc_region_dr_of(us, 10, 125, false) == 0 && lwc_region_dr_of(us, 10, 125, true) == -1);
    CHECK(lwc_region_dr_of(us, 12, 500, true) == 8 && lwc_region_dr_of(us, 12, 125, false) == -1);
    CHECK(lwc_region_max_n(eu, -1, false) == 0U && lwc_region_max_n(eu, 16, false) == 0U);
    CHECK(lwc_region_rx1_dr(eu, 16, 0, false) == -1 && lwc_region_rx1_dr(eu, 0, 8, false) == -1);
}

/* Every region: what the tables say must hang together. */
static void consistency(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = R((lwc_region_id_t)i);
        CHECK(r != NULL && r->id == (lwc_region_id_t)i && r->tx_powers >= 8U);
        CHECK(r->dr[r->rx2_dr].kind == LWC_DR_LORA && r->rx2_hz != 0U);
        for (int dr = 0; dr < 16; dr++) {
            const lwc_region_dr_t *d = &r->dr[dr];
            if (d->kind == LWC_DR_LORA) {
                CHECK(d->sf >= 7U && d->sf <= 12U && (d->bw_khz == 125U || d->bw_khz == 250U || d->bw_khz == 500U));
                CHECK(d->n > 0U && d->n <= 222U && d->n_dwell <= d->n);
            }
            for (uint8_t off = 0; off < 8U; off++) {
                for (int dw = 0; dw <= 1; dw++) {
                    const int rx = lwc_region_rx1_dr(r, dr, off, dw != 0);
                    CHECK(rx < 0 || r->dr[rx].kind != LWC_DR_NONE);
                }
            }
        }
        if (r->fixed) {
            for (uint8_t sb = 0; sb < 8U; sb++) { /* every channel of a sub-band answers on a downlink channel */
                for (uint32_t k = 0; k < 8U; k++) {
                    const uint32_t up = r->up125.base_hz + (8U * sb + k) * r->up125.step_hz;
                    CHECK(lwc_region_rx1_hz(r, up, 125) == r->down.base_hz + k * r->down.step_hz);
                }
            }
        } else {
            CHECK(r->n_default >= 2U && r->n_default <= 3U);
            for (unsigned c = 0; c < r->n_default; c++) {
                CHECK(r->default_ch[c].hz != 0U && r->dr[r->default_ch[c].max_dr].kind == LWC_DR_LORA);
            }
            uint8_t cf[16];
            uint16_t m[2];
            uint8_t cn[2];
            CHECK(!lwc_region_cflist_subband(r, 0, cf) && !lwc_region_link_adr_subband(r, 0, m, cn));
        }
    }
}

/* The fixed plans' sub-band rules, against the reference (core/test/region_ref.inc). */
typedef struct {
    const char *region;
    const char *proto;
    uint8_t sb;
    uint8_t nmask;
    uint16_t mask[5];
    uint8_t ncmd;
    struct {
        uint16_t mask;
        uint8_t cntl;
    } cmd[2];
} ref_t;

static const ref_t REF[] = {
#include "region_ref.inc"
};

static void subbands(void)
{
    unsigned checked = 0;
    for (size_t i = 0; i < sizeof REF / sizeof REF[0]; i++) {
        const ref_t *x = &REF[i];
        lwc_region_id_t id;
        CHECK(lwc_region_parse(x->region, &id));
        const lwc_region_t *r = R(id);
        uint16_t m[2];
        uint8_t cn[2];
        CHECK(lwc_region_link_adr_subband(r, x->sb, m, cn));
        CHECK(x->ncmd == 2U && m[0] == x->cmd[0].mask && cn[0] == x->cmd[0].cntl && m[1] == x->cmd[1].mask &&
              cn[1] == x->cmd[1].cntl);
        uint8_t cf[16];
        CHECK(lwc_region_cflist_subband(r, x->sb, cf));
        if (x->nmask == 5U) { /* 1.0.4: the channel-mask CFList */
            for (unsigned k = 0; k < 5U; k++) {
                CHECK((uint16_t)(cf[2U * k] | (cf[2U * k + 1U] << 8)) == x->mask[k]);
            }
            CHECK(cf[15] == 1U && cf[10] == 0U && cf[14] == 0U);
        } else {
            CHECK(strcmp(x->proto, "1.0.2") == 0 && x->nmask == 0U); /* 1.0.2: no CFList; LinkADRReq after the join */
        }
        checked++;
    }
    CHECK(checked == 32U);
    uint8_t cf[16];
    uint16_t m[2];
    uint8_t cn[2];
    CHECK(!lwc_region_cflist_subband(R(LWC_REGION_US915), 8, cf) && !lwc_region_link_adr_subband(R(LWC_REGION_US915), 8, m, cn));
}

/* ADR-0024: what the server code assumes of every dynamic-channel region. */
static void dynamic_rules(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = R((lwc_region_id_t)i);
        CHECK(lwc_region_tx_enabled(r) == (r->id == LWC_REGION_EU868)); /* receive-only elsewhere */
        if (r->fixed) {
            continue;
        }
        for (int dr = 0; dr <= 5; dr++) { /* ADR and the RX2 window use DR = 12 - SF at 125 kHz */
            CHECK(r->dr[dr].kind == LWC_DR_LORA && r->dr[dr].sf == 12 - dr && r->dr[dr].bw_khz == 125U);
            CHECK(lwc_region_rx1_dr(r, dr, 0, false) == dr); /* RX1DROffset 0: the uplink's DR */
        }
        CHECK(r->rx2_dr <= 5U);
        for (unsigned c = 1; c < r->n_default; c++) {
            CHECK(r->default_ch[c].hz > r->default_ch[c - 1U].hz); /* lowest first: the default plan relies on it */
        }
    }
    CHECK(!lwc_region_tx_enabled(NULL));
}

int main(void)
{
    dynamic_rules();
    spot_values();
    lookup();
    consistency();
    subbands();
    if (failures != 0) {
        fprintf(stderr, "test_region: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_region: all passed\n");
    return 0;
}
