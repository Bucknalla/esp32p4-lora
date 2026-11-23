/* Radio channel plan (ADR-0017). */
#include "lwc/plan.h"

#include <stdio.h>
#include <string.h>

#include "lwc/ns.h"

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
    lwc_plan_default_eu868(&p);
    CHECK(lwc_plan_check(&p) == LWC_PLAN_OK);
    /* the channels as conc_hal.c set them before the plan existed */
    static const uint32_t want[8] = {868100000, 868300000, 868500000, 867100000,
                                     867300000, 867500000, 867700000, 867900000};
    for (size_t i = 0; i < 8; i++) {
        CHECK(lwc_plan_hz(&p, &p.multisf[i]) == want[i]);
    }
    CHECK(lwc_plan_hz(&p, &p.std) == 868300000 && p.std_sf == 7 && p.std_bw_khz == 250);
    CHECK(lwc_plan_hz(&p, &p.fsk) == 0); /* off */
    uint32_t cf[5];
    CHECK(lwc_plan_cflist(&p, cf) == 5);
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
    lwc_plan_default_eu868(&p);
    p.multisf[7].if_hz = 400001; /* 125 kHz channel: |IF| <= 400 kHz */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_IF);
    lwc_plan_default_eu868(&p);
    p.multisf[3].radio = 2;
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_IF);
    lwc_plan_default_eu868(&p);
    p.std.if_hz = -400000; /* 250 kHz channel: |IF| <= 375 kHz */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_IF);
    p.std.if_hz = -375000;
    CHECK(lwc_plan_check(&p) == LWC_PLAN_OK);
    lwc_plan_default_eu868(&p);
    p.radio_hz[0] = 862000000;
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_RADIO);
    lwc_plan_default_eu868(&p);
    p.radio_hz[1] = 869700000; /* 868.1-868.5 move to 869.3-869.7: the defaults are missing */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_DEFAULT);
    p.radio_hz[1] = 869950000; /* the top channel at 869.95 MHz reaches past 870 MHz */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_BAND);
    lwc_plan_default_eu868(&p);
    p.multisf[0].enable = false; /* 868.1 MHz gone */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_DEFAULT);
    lwc_plan_default_eu868(&p);
    p.std_sf = 6;
    CHECK(lwc_plan_check(&p) == LWC_PLAN_ERR_STD);
    p.std.enable = false; /* a disabled std channel is not checked */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_OK);
    lwc_plan_default_eu868(&p);
    p.fsk.enable = true; /* 868.8 MHz */
    CHECK(lwc_plan_check(&p) == LWC_PLAN_OK && lwc_plan_hz(&p, &p.fsk) == 868800000);
    CHECK(lwc_plan_check(NULL) == LWC_PLAN_ERR_ARG);
}

static void cflist(void)
{
    lwc_plan_t p;
    lwc_plan_default_eu868(&p);
    /* unsorted, a duplicate, a disabled one: the list comes out sorted and unique */
    p.multisf[3].if_hz = 400000;  /* 867.9 */
    p.multisf[7].if_hz = -400000; /* 867.1 */
    p.multisf[6].if_hz = -400000; /* 867.1 again */
    p.multisf[5].enable = false;
    uint32_t cf[5];
    CHECK(lwc_plan_cflist(&p, cf) == 3);
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
    lwc_plan_default_eu868(&p);
    p.fsk.enable = true;
    p.multisf[6].if_hz = -300000;
    uint8_t b[LWC_PLAN_PACKED + 4];
    CHECK(lwc_plan_pack(&p, b, sizeof b) && b[0] == LWC_PLAN_VERSION);
    CHECK(lwc_plan_unpack(b, LWC_PLAN_PACKED, &q) && lwc_plan_equal(&p, &q) && q.std_bw_khz == 250);
    CHECK(!lwc_plan_pack(&p, b, LWC_PLAN_PACKED - 1));
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED - 1, &q)); /* wrong length */
    b[0] = 2;
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, &q)); /* another version */
    b[0] = LWC_PLAN_VERSION;
    b[9] = 3; /* first channel's enable byte: not 0 or 1 */
    CHECK(!lwc_plan_unpack(b, LWC_PLAN_PACKED, &q));
    p.multisf[0].enable = false; /* a plan that fails the checks is not read back */
    CHECK(lwc_plan_pack(&p, b, sizeof b) && !lwc_plan_unpack(b, LWC_PLAN_PACKED, &q));
}

int main(void)
{
    defaults();
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
