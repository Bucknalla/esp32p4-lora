/* Radio channel plan (ADR-0017). */
#include "lwc/plan.h"

#include <stdio.h>
#include <string.h>

#include "lwc/ns.h"

#include "lwc/region.h"
#define EU lwc_region(LWC_REGION_EU868)

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
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_OK);
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
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_IF);
    (void)lwc_plan_default(&p, EU);
    p.multisf[3].radio = 2;
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_IF);
    (void)lwc_plan_default(&p, EU);
    p.std.if_hz = -400000; /* 250 kHz channel: |IF| <= 375 kHz */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_IF);
    p.std.if_hz = -375000;
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_OK);
    (void)lwc_plan_default(&p, EU);
    p.radio_hz[0] = 862000000;
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_RADIO);
    (void)lwc_plan_default(&p, EU);
    p.radio_hz[1] = 869700000; /* 868.1-868.5 move to 869.3-869.7: the defaults are missing */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_DEFAULT);
    p.radio_hz[1] = 869950000; /* the top channel at 869.95 MHz reaches past 870 MHz */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_BAND);
    (void)lwc_plan_default(&p, EU);
    p.multisf[0].enable = false; /* 868.1 MHz gone */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_DEFAULT);
    (void)lwc_plan_default(&p, EU);
    p.std_sf = 6;
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_ERR_STD);
    p.std.enable = false; /* a disabled std channel is not checked */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_OK);
    (void)lwc_plan_default(&p, EU);
    p.fsk.enable = true; /* 868.8 MHz */
    CHECK(lwc_plan_check(&p, EU) == LWC_PLAN_OK && lwc_plan_hz(&p, &p.fsk) == 868800000);
    CHECK(lwc_plan_check(NULL, EU) == LWC_PLAN_ERR_ARG);
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
    CHECK(lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, &q) && lwc_plan_equal(&p, &q) && q.std_bw_khz == 250);
    CHECK(!lwc_plan_pack(&p, b, LWC_PLAN_PACKED - 1));
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED - 1, EU, &q)); /* wrong length */
    b[0] = 2;
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, &q)); /* another version */
    b[0] = LWC_PLAN_VERSION;
    b[9] = 3; /* first channel's enable byte: not 0 or 1 */
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, &q));
    p.multisf[0].enable = false; /* a plan that fails the checks is not read back */
    CHECK(lwc_plan_pack(&p, b, sizeof b) && !lwc_plan_unpack(b, LWC_PLAN_PACKED, EU, &q));
}

/* ADR-0024: every dynamic region has a default plan that passes its own check. */
static void regions(void)
{
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
        lwc_plan_t p;
        if (r->fixed) {
            CHECK(!lwc_plan_default(&p, r) && lwc_plan_check(&p, r) == LWC_PLAN_ERR_REGION);
            uint32_t cf[5];
            CHECK(lwc_plan_cflist(&p, r, cf) == 0);
            continue;
        }
        CHECK(lwc_plan_default(&p, r) && lwc_plan_check(&p, r) == LWC_PLAN_OK);
        for (unsigned k = 0; k < r->n_default; k++) {
            bool found = false;
            for (size_t c = 0; c < LWC_PLAN_MULTISF; c++) {
                found = found || lwc_plan_hz(&p, &p.multisf[c]) == r->default_ch[k].hz;
            }
            CHECK(found);
        }
        uint32_t cf[5];
        CHECK(lwc_plan_cflist(&p, r, cf) == (r->id == LWC_REGION_EU868 ? 5U : 0U));
        uint8_t b[LWC_PLAN_PACKED];
        lwc_plan_t q;
        CHECK(lwc_plan_pack(&p, b, sizeof b) && lwc_plan_unpack(b, sizeof b, r, &q) && lwc_plan_equal(&p, &q));
        if (r->id != LWC_REGION_EU868) {
            CHECK(!p.std.enable && !p.fsk.enable);
            /* EU868's plan in another region: its default channels are missing */
            lwc_plan_t eu;
            (void)lwc_plan_default(&eu, EU);
            CHECK(lwc_plan_check(&eu, r) == LWC_PLAN_ERR_DEFAULT && !lwc_plan_unpack(b, sizeof b, EU, &q));
        }
    }
    /* IN865's defaults span 922.5 kHz: more than one radio's reach, so two radios */
    lwc_plan_t p;
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_IN865)) && p.radio_hz[0] != p.radio_hz[1]);
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_AS923_1)) && p.radio_hz[0] == 923300000U &&
          p.radio_hz[1] == p.radio_hz[0] && p.multisf[0].if_hz == -100000 && p.multisf[1].if_hz == 100000 &&
          !p.multisf[2].enable);
    /* outside EU868 no band edges yet (ADR-0024): a radio at 923 MHz is not refused for KR920 */
    CHECK(lwc_plan_default(&p, lwc_region(LWC_REGION_KR920)) && p.radio_hz[0] == 922300000U);
    CHECK(lwc_plan_check(&p, NULL) == LWC_PLAN_ERR_ARG && !lwc_plan_default(&p, NULL));
}

int main(void)
{
    defaults();
    regions();
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
