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
    /* CN470 (LoRaWAN 1.0.2rB: 96 uplink, 48 downlink channels) and EU433 */
    const lwc_region_t *cn = R(LWC_REGION_CN470), *e4 = R(LWC_REGION_EU433);
    CHECK(cn && strcmp(cn->name, "CN470") == 0 && cn->fixed && cn->rx2_hz == 505300000U && cn->rx2_dr == 0);
    CHECK(cn->n_up125 == 96 && cn->n_up500 == 0 && cn->n_down == 48 && lwc_region_subbands(cn) == 12U);
    CHECK(lwc_region_max_n(cn, 0, false) == 51U && lwc_region_max_n(cn, 3, false) == 115U && cn->dr[6].kind == LWC_DR_NONE);
    CHECK(lwc_region_channel(cn, 470300000U, 125) == 0 && lwc_region_channel(cn, 489300000U, 125) == 95);
    CHECK(lwc_region_channel(cn, 489500000U, 125) == -1 && lwc_region_channel(cn, 470300000U, 500) == -1);
    CHECK(lwc_region_rx1_hz(cn, 470300000U, 125) == 500300000U && lwc_region_rx1_hz(cn, 489300000U, 125) == 509700000U);
    CHECK(lwc_region_rx1_hz(cn, 479900000U, 125) == 500300000U); /* channel 48: downlink channel 0 */
    CHECK(lwc_region_dr_of(cn, 12, 125, true) == 0 && lwc_region_dr_of(cn, 7, 125, false) == 5); /* same DRs both ways */
    CHECK(lwc_region_rx1_dr(cn, 5, 2, false) == 3 && !cn->tx_param_setup);
    CHECK(e4 && strcmp(e4->name, "EU433") == 0 && !e4->fixed && e4->n_default == 3 && e4->rx2_hz == 434665000U);
    CHECK(e4->default_ch[0].hz == 433175000U && e4->default_ch[2].hz == 433575000U && e4->max_eirp_dbm == 12);
    CHECK(e4->dr[6].sf == 7 && e4->dr[6].bw_khz == 250 && e4->tx_powers == 6 && lwc_region_max_n(e4, 0, false) == 51U);
    CHECK(lwc_region_subbands(us) == 8U && lwc_region_subbands(au) == 8U && lwc_region_subbands(eu) == 0U);
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
        CHECK(r != NULL && r->id == (lwc_region_id_t)i && r->tx_powers >= 6U); /* EU433: TXPower 0-5 */
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
            /* every uplink channel answers on downlink channel (channel % n_down) */
            CHECK(r->n_up125 % 8U == 0U && r->n_down > 0U && lwc_region_subbands(r) == r->n_up125 / 8U);
            for (uint32_t ch = 0; ch < (uint32_t)r->n_up125 + r->n_up500; ch++) {
                const bool wide = ch >= r->n_up125;
                const uint32_t up = wide ? r->up500.base_hz + (ch - r->n_up125) * r->up500.step_hz
                                         : r->up125.base_hz + ch * r->up125.step_hz;
                CHECK(lwc_region_channel(r, up, wide ? 500 : 125) == (int)ch);
                CHECK(lwc_region_rx1_hz(r, up, wide ? 500 : 125) == r->down.base_hz + (ch % r->n_down) * r->down.step_hz);
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
    /* CN470's original plan has no CFList, and moving it to a sub-band takes 6 LinkADRReq: not provided */
    CHECK(!lwc_region_cflist_subband(R(LWC_REGION_CN470), 0, cf) && !lwc_region_link_adr_subband(R(LWC_REGION_CN470), 0, m, cn));
}

/* ADR-0025: which card serves which region, and the band check. */
static void cards(void)
{
    static const struct {
        lwc_card_id_t card;
        const char *regions;
    } want[] = {
        {LWC_CARD_868, "EU868 IN865 RU864"},
        {LWC_CARD_915, "US915 AU915 AS923-1 AS923-2 AS923-3 AS923-4 KR920"},
        {LWC_CARD_433, "EU433"},
        {LWC_CARD_470, "CN470"},
    };
    int served[LWC_REGIONS] = {0};
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        const lwc_card_t *c = lwc_card(want[k].card);
        char got[128] = "";
        for (int i = 0; i < (int)LWC_REGIONS; i++) {
            if (lwc_card_region_ok(c, R((lwc_region_id_t)i))) {
                if (got[0] != '\0') {
                    strcat(got, " ");
                }
                strcat(got, R((lwc_region_id_t)i)->name);
                served[i]++;
            }
        }
        CHECK(strcmp(got, want[k].regions) == 0);
        lwc_card_id_t id;
        CHECK(c != NULL && c->id == want[k].card && lwc_card_parse(c->name, &id) && id == want[k].card);
    }
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        CHECK(served[i] == 1); /* every region: exactly one card */
        const lwc_card_t *c = lwc_card_for_region(R((lwc_region_id_t)i));
        CHECK(c != NULL && lwc_card_region_ok(c, R((lwc_region_id_t)i)));
    }
    /* an upgrade from ADR-0024 keeps a 9xx region on the 915 card (not EU868 on the 868 card) */
    CHECK(lwc_card_for_region(R(LWC_REGION_AS923_1))->id == LWC_CARD_915);
    CHECK(lwc_card_for_region(R(LWC_REGION_KR920))->id == LWC_CARD_915);
    CHECK(lwc_card_for_region(R(LWC_REGION_EU868))->id == LWC_CARD_868 && lwc_card_for_region(NULL) == NULL);
    const lwc_card_t *c868 = lwc_card(LWC_CARD_868), *c433 = lwc_card(LWC_CARD_433);
    CHECK(c868->radio == LWC_RADIO_SX1257 && lwc_card(LWC_CARD_915)->radio == LWC_RADIO_SX1257);
    CHECK(c433->radio == LWC_RADIO_SX1255 && lwc_card(LWC_CARD_470)->radio == LWC_RADIO_SX1255);
    /* band edges: the whole channel inside */
    CHECK(lwc_card_fits(c868, 863062500U, 125000U) && !lwc_card_fits(c868, 863062499U, 125000U));
    CHECK(lwc_card_fits(c868, 869937500U, 125000U) && !lwc_card_fits(c868, 869937501U, 125000U));
    CHECK(lwc_card_fits(c433, 434727500U, 125000U) && !lwc_card_fits(c433, 434775000U, 125000U));
    CHECK(!lwc_card_fits(c868, 10U, 125000U) && !lwc_card_fits(NULL, 868100000U, 125000U));
    lwc_card_id_t id;
    CHECK(!lwc_card_parse("900", &id) && !lwc_card_parse(NULL, &id) && lwc_card(LWC_CARDS) == NULL);
    CHECK(!lwc_card_region_ok(NULL, R(LWC_REGION_EU868)) && !lwc_card_region_ok(c868, NULL));
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
    cards();
    if (failures != 0) {
        fprintf(stderr, "test_region: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_region: all passed\n");
    return 0;
}
