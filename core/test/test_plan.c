/* Radio channel plan (ADR-0017). */
#include "lwc/plan.h"

#include <stdio.h>
#include <string.h>

#include "lwc/ns.h"

#include "lwc/region.h"
#define EU lwc_region(LWC_REGION_EU868)
#define C868 lwc_card(LWC_CARD_868)

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void defaults(void)
{
    lwc_plan_t p;
    (void)lwc_plan_default(&p, EU);
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_OK);
    /* the channels as conc_hal.c set them before the plan existed */
    static const uint32_t want[8] = {868100000, 868300000, 868500000, 867100000,
                                     867300000, 867500000, 867700000, 867900000};
    for (size_t i = 0; i < 8; i++) {
        CHECK(lwc_plan_hz(&p, &p.multisf[i]) == want[i]);
    }
    CHECK(lwc_plan_hz(&p, &p.std) == 868300000 && p.std_sf == 7 && p.std_bw_khz == 250);
    CHECK(lwc_plan_hz(&p, &p.fsk) == 0); /* off */
    uint32_t cf[5];
    CHECK(lwc_plan_cflist(&p, EU, cf) == 5);
    CHECK(cf[0] == 867100000 && cf[1] == 867300000 && cf[2] == 867500000 && cf[3] == 867700000 && cf[4] == 867900000);
    lwc_plan_t q = p;
    CHECK(lwc_plan_equal(&p, &q));
    q.fsk.if_hz = 1; /* a disabled channel's settings do not count */
    CHECK(lwc_plan_equal(&p, &q));
    q.multisf[7].if_hz = 300000;
    CHECK(!lwc_plan_equal(&p, &q));
}

static void checks(void)
{
    lwc_plan_t p;
    (void)lwc_plan_default(&p, EU);
    p.multisf[7].if_hz = 400001; /* 125 kHz channel: |IF| <= 400 kHz */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_IF);
    (void)lwc_plan_default(&p, EU);
    p.multisf[3].radio = 2;
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_IF);
    (void)lwc_plan_default(&p, EU);
    p.std.if_hz = -400000; /* 250 kHz channel: |IF| <= 375 kHz */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_IF);
    p.std.if_hz = -375000;
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_OK);
    (void)lwc_plan_default(&p, EU);
    p.radio_hz[0] = 862000000;
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_RADIO);
    (void)lwc_plan_default(&p, EU);
    p.radio_hz[1] = 869700000; /* 868.1-868.5 move to 869.3-869.7: the defaults are missing */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_DEFAULT);
    p.radio_hz[1] = 869950000; /* the top channel at 869.95 MHz reaches past 870 MHz */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_BAND);
    (void)lwc_plan_default(&p, EU);
    p.multisf[0].enable = false; /* 868.1 MHz gone */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_DEFAULT);
    (void)lwc_plan_default(&p, EU);
    p.std_sf = 6;
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_ERR_STD);
    p.std.enable = false; /* a disabled std channel is not checked */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_OK);
    (void)lwc_plan_default(&p, EU);
    p.fsk.enable = true; /* 868.8 MHz */
    CHECK(lwc_plan_check(&p, EU, C868) == LWC_PLAN_OK && lwc_plan_hz(&p, &p.fsk) == 868800000);
    CHECK(lwc_plan_check(NULL, EU, C868) == LWC_PLAN_ERR_ARG);
}

static void cflist(void)
{
    lwc_plan_t p;
    (void)lwc_plan_default(&p, EU);
    /* unsorted, a duplicate, a disabled one: the list comes out sorted and unique */
    p.multisf[3].if_hz = 400000;  /* 867.9 */
    p.multisf[7].if_hz = -400000; /* 867.1 */
    p.multisf[6].if_hz = -400000; /* 867.1 again */
    p.multisf[5].enable = false;
    uint32_t cf[5];
    CHECK(lwc_plan_cflist(&p, EU, cf) == 3);
    CHECK(cf[0] == 867100000 && cf[1] == 867300000 && cf[2] == 867900000 && cf[3] == 0 && cf[4] == 0);
    /* the local server's join-accepts follow the plan */
    static lwc_ns_t ns;
    lwc_ns_init(&ns);
    CHECK(ns.cflist_hz[0] == 867100000 && ns.cflist_hz[4] == 867900000);
    lwc_ns_set_plan(&ns, &p);
    CHECK(ns.cflist_hz[2] == 867900000 && ns.cflist_hz[3] == 0);
}

static void stored(void)
{
    lwc_plan_t p, q;
    (void)lwc_plan_default(&p, EU);
    p.fsk.enable = true;
    p.multisf[6].if_hz = -300000;
    uint8_t b[LWC_PLAN_PACKED + 4];
    CHECK(lwc_plan_pack(&p, b, sizeof b) && b[0] == LWC_PLAN_VERSION);
    CHECK(lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, C868, &q) && lwc_plan_equal(&p, &q) && q.std_bw_khz == 250);
    CHECK(!lwc_plan_pack(&p, b, LWC_PLAN_PACKED - 1));
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED - 1, EU, C868, &q)); /* wrong length */
    b[0] = 2;
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, C868, &q)); /* another version */
    b[0] = LWC_PLAN_VERSION;
    b[9] = 3; /* first channel's enable byte: not 0 or 1 */
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, C868, &q));
    p.multisf[0].enable = false; /* a plan that fails the checks is not read back */
    CHECK(lwc_plan_pack(&p, b, sizeof b) && !lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, C868, &q));
}

/* The card that serves a region (ADR-0025: exactly one). */
static const lwc_card_t *card_for(const lwc_region_t *r)
{
    for (int c = 0; c < (int)LWC_CARDS; c++) {
        if (lwc_card_region_ok(lwc_card((lwc_card_id_t)c), r)) {
            return lwc_card((lwc_card_id_t)c);
        }
    }
    return NULL;
}

/* ADR-0024, ADR-0025: every region has a default plan that passes its own check. */
static void regions(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
        const lwc_card_t *card = card_for(r);
        lwc_plan_t p;
        CHECK(card != NULL && lwc_plan_default(&p, r) && lwc_plan_check(&p, r, card) == LWC_PLAN_OK);
        uint8_t b[LWC_PLAN_PACKED];
        lwc_plan_t q;
        CHECK(lwc_plan_pack(&p, b, sizeof b) && lwc_plan_unpack(b, sizeof b, r, card, &q) && lwc_plan_equal(&p, &q));
        /* on a card that cannot serve the region */
        const lwc_card_t *other = lwc_card(card->id == LWC_CARD_868 ? LWC_CARD_915 : LWC_CARD_868);
        CHECK(lwc_plan_check(&p, r, other) == LWC_PLAN_ERR_REGION && !lwc_plan_unpack(b, sizeof b, r, other, &q));
        uint32_t cf[5];
        if (r->fixed) {
            CHECK(lwc_plan_cflist(&p, r, cf) == 0 && !p.fsk.enable);
            CHECK(lwc_plan_subband_of(&p, r) == (int)lwc_plan_default_subband(r));
            for (uint8_t sb = 0; sb < lwc_region_subbands(r); sb++) {
                CHECK(lwc_plan_subband(&p, r, sb) && lwc_plan_check(&p, r, card) == LWC_PLAN_OK);
                CHECK(lwc_plan_subband_of(&p, r) == (int)sb && p.std.enable == (r->n_up500 > 0U));
            }
            CHECK(!lwc_plan_subband(&p, r, lwc_region_subbands(r)));
            continue;
        }
        CHECK(lwc_plan_subband_of(&p, r) == -1 && !lwc_plan_subband(&p, r, 0));
        CHECK(lwc_plan_default(&p, r));
        for (unsigned k = 0; k < r->n_default; k++) {
            bool found = false;
            for (size_t c = 0; c < LWC_PLAN_MULTISF; c++) {
                found = found || lwc_plan_hz(&p, &p.multisf[c]) == r->default_ch[k].hz;
            }
            CHECK(found);
        }
        CHECK(lwc_plan_cflist(&p, r, cf) == (r->id == LWC_REGION_EU868 ? 5U : 0U));
        if (r->id != LWC_REGION_EU868) {
            CHECK(!p.std.enable && !p.fsk.enable);
            /* EU868's plan in another region: its default channels are missing (checked
             * before the card's band) */
            lwc_plan_t eu;
            (void)lwc_plan_default(&eu, EU);
            CHECK(lwc_plan_check(&eu, r, card) == LWC_PLAN_ERR_DEFAULT && !lwc_plan_unpack(b, sizeof b, EU, C868, &q));
        }
    }
    /* IN865's defaults span 922.5 kHz: more than one radio's reach, so two radios */
    lwc_plan_t p;
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_IN865)) && p.radio_hz[0] != p.radio_hz[1]);
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_AS923_1)) && p.radio_hz[0] == 923300000U &&
          p.radio_hz[1] == p.radio_hz[0] && p.multisf[0].if_hz == -100000 && p.multisf[1].if_hz == 100000 &&
          !p.multisf[2].enable);
    /* outside EU868 no region band edges yet (R4): on the 915 card a radio at 923 MHz is not refused for KR920 */
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_KR920)) && p.radio_hz[0] == 922300000U);
    CHECK(lwc_plan_check(&p, NULL, C868) == LWC_PLAN_ERR_ARG && lwc_plan_check(&p, EU, NULL) == LWC_PLAN_ERR_ARG);
    CHECK(!lwc_plan_default(&p, NULL));
}

/* ADR-0025: the fixed plans, against the TTN frequency plans (lorawan-frequency-plans:
 * US_902_928_FSB_2, AU_915_928_FSB_2, CN_470_510_FSB_11) and ChirpStack's US915 test. */
static void fixed_plans(void)
{
    const lwc_region_t *us = lwc_region(LWC_REGION_US915), *au = lwc_region(LWC_REGION_AU915);
    const lwc_region_t *cn = lwc_region(LWC_REGION_CN470);
    const lwc_card_t *c915 = lwc_card(LWC_CARD_915), *c470 = lwc_card(LWC_CARD_470);
    lwc_plan_t p;
    /* US915 FSB2: radios 904.3 / 905.0 MHz, 903.9-905.3 MHz, std 904.6 MHz SF8 500 kHz on radio 0 */
    CHECK(lwc_plan_default(&p, us) && p.radio_hz[0] == 904300000U && p.radio_hz[1] == 905000000U);
    for (unsigned k = 0; k < 8U; k++) {
        CHECK(p.multisf[k].enable && p.multisf[k].radio == (k < 4U ? 0 : 1) &&
              lwc_plan_hz(&p, &p.multisf[k]) == 903900000U + k * 200000U);
    }
    CHECK(p.std.enable && p.std.radio == 0 && lwc_plan_hz(&p, &p.std) == 904600000U && p.std_sf == 8 &&
          p.std_bw_khz == 500 && !p.fsk.enable);
    /* AU915 FSB2: radios 917.2 / 917.9 MHz, 916.8-918.2 MHz, std 917.5 MHz SF8 500 kHz (DR6) */
    CHECK(lwc_plan_default(&p, au) && p.radio_hz[0] == 917200000U && p.radio_hz[1] == 917900000U);
    CHECK(lwc_plan_hz(&p, &p.multisf[0]) == 916800000U && lwc_plan_hz(&p, &p.multisf[7]) == 918200000U);
    CHECK(lwc_plan_hz(&p, &p.std) == 917500000U && p.std_sf == 8 && p.std_bw_khz == 500);
    /* CN470 FSB11: radios 486.6 / 487.4 MHz, 486.3-487.7 MHz, no 500 kHz channel */
    CHECK(lwc_plan_default(&p, cn) && p.radio_hz[0] == 486600000U && p.radio_hz[1] == 487400000U);
    CHECK(lwc_plan_hz(&p, &p.multisf[0]) == 486300000U && lwc_plan_hz(&p, &p.multisf[7]) == 487700000U);
    CHECK(!p.std.enable && lwc_plan_check(&p, cn, c470) == LWC_PLAN_OK);
    /* ChirpStack's US915 layout (sub-band 1: 902.3-903.7 MHz): 5 channels on radio 0 */
    lwc_plan_t cs;
    memset(&cs, 0, sizeof cs);
    cs.radio_hz[0] = 902700000U;
    cs.radio_hz[1] = 903700000U;
    static const int32_t ifs[8] = {-400000, -200000, 0, 200000, 400000, -400000, -200000, 0};
    for (unsigned k = 0; k < 8U; k++) {
        cs.multisf[k] = (lwc_plan_chan_t){true, (uint8_t)(k < 5U ? 0 : 1), ifs[k]};
    }
    cs.std = (lwc_plan_chan_t){true, 0, 300000};
    cs.std_sf = 8;
    cs.std_bw_khz = 500;
    CHECK(lwc_plan_check(&cs, us, c915) == LWC_PLAN_OK && lwc_plan_subband_of(&cs, us) == 0);
    /* not a channel of the grid, FSK on, std at another bandwidth or SF */
    lwc_plan_t b = cs;
    b.multisf[2].if_hz = 100000;
    CHECK(lwc_plan_check(&b, us, c915) == LWC_PLAN_ERR_CHANNEL && lwc_plan_subband_of(&b, us) == -1);
    b = cs;
    b.fsk = (lwc_plan_chan_t){true, 1, 100000};
    CHECK(lwc_plan_check(&b, us, c915) == LWC_PLAN_ERR_CHANNEL);
    b = cs;
    b.std_bw_khz = 250;
    CHECK(lwc_plan_check(&b, us, c915) == LWC_PLAN_ERR_CHANNEL);
    b = cs;
    b.std_sf = 7; /* SF7 500 kHz is a downlink DR only in US915 */
    CHECK(lwc_plan_check(&b, us, c915) == LWC_PLAN_ERR_STD);
    b = cs;
    b.std.enable = false; /* the 500 kHz channel may be off */
    CHECK(lwc_plan_check(&b, us, c915) == LWC_PLAN_OK);
    CHECK(lwc_plan_default(&p, cn));
    p.std = (lwc_plan_chan_t){true, 0, 0};
    p.std_bw_khz = 500;
    CHECK(lwc_plan_check(&p, cn, c470) == LWC_PLAN_ERR_CHANNEL); /* CN470: no 500 kHz channels */
    /* US915 plan on the 868 card; EU868 plan on the 915 card */
    CHECK(lwc_plan_check(&cs, us, C868) == LWC_PLAN_ERR_REGION);
    (void)lwc_plan_default(&p, EU);
    CHECK(lwc_plan_check(&p, EU, c915) == LWC_PLAN_ERR_REGION);
}

/* ADR-0025: the card's band is checked last, after everything else. */
static void card_band(void)
{
    const lwc_region_t *e4 = lwc_region(LWC_REGION_EU433);
    const lwc_card_t *c433 = lwc_card(LWC_CARD_433);
    /* TTN EU_433: radios 433.475 / 434.275 MHz, 433.175-434.575 MHz, std 434.075 MHz SF7 250 kHz */
    lwc_plan_t p;
    memset(&p, 0, sizeof p);
    p.radio_hz[0] = 433475000U;
    p.radio_hz[1] = 434275000U;
    static const int32_t ifs[8] = {-300000, -100000, 100000, 300000, -300000, -100000, 100000, 300000};
    for (unsigned k = 0; k < 8U; k++) {
        p.multisf[k] = (lwc_plan_chan_t){true, (uint8_t)(k < 4U ? 0 : 1), ifs[k]};
    }
    p.std = (lwc_plan_chan_t){true, 1, -200000};
    p.std_sf = 7;
    p.std_bw_khz = 250;
    CHECK(lwc_plan_check(&p, e4, c433) == LWC_PLAN_OK);
    p.multisf[7].if_hz = 400000; /* 434.675 MHz: its upper edge 434.7375 MHz, inside */
    CHECK(lwc_plan_check(&p, e4, c433) == LWC_PLAN_OK);
    p.radio_hz[1] = 434375000U; /* moves radio 1's channels up 100 kHz: 434.775 MHz reaches 434.8375 */
    CHECK(lwc_plan_check(&p, e4, c433) == LWC_PLAN_ERR_CARD);
    for (unsigned k = 4; k < 8U; k++) {
        p.multisf[k].enable = false;
    }
    p.radio_hz[1] = 434400000U;
    p.std.if_hz = 375000; /* std 434.775 MHz, 250 kHz wide: reaches 434.9 MHz */
    CHECK(lwc_plan_check(&p, e4, c433) == LWC_PLAN_ERR_CARD);
    p.std.if_hz = 200000; /* 434.6 MHz: up to 434.725 MHz */
    CHECK(lwc_plan_check(&p, e4, c433) == LWC_PLAN_OK);
}

int main(void)
{
    defaults();
    regions();
    fixed_plans();
    card_band();
    stored();
    checks();
    cflist();
    if (failures != 0) {
        fprintf(stderr, "test_plan: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_plan: all passed\n");
    return 0;
}
