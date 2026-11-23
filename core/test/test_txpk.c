#include "lwc/txpk.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_jit(void)
{
    lwc_jit_t q;
    lwc_jit_init(&q, 20000, 10000000, 1000);
    size_t s0, s1;
    const uint32_t now = 1000000;
    CHECK(lwc_jit_admit(&q, now, now + 10000, 50000, 1, &s0) == LWC_JIT_TOO_LATE);
    CHECK(lwc_jit_admit(&q, now, now + 11000000, 50000, 1, &s0) == LWC_JIT_TOO_EARLY);
    CHECK(lwc_jit_admit(&q, now, now + 1000000, 50000, 1, &s0) == LWC_JIT_OK);
    /* overlaps the first (starts 30 ms in) */
    CHECK(lwc_jit_admit(&q, now, now + 1030000, 50000, 2, &s1) == LWC_JIT_COLLISION);
    /* ends just before the first starts, but inside the guard */
    CHECK(lwc_jit_admit(&q, now, now + 949500, 50000, 2, &s1) == LWC_JIT_COLLISION);
    CHECK(lwc_jit_admit(&q, now, now + 2000000, 50000, 2, &s1) == LWC_JIT_OK);

    int expired;
    CHECK(lwc_jit_due(&q, now, 40000, 5000, &expired) == -1 && expired == -1);
    CHECK(lwc_jit_due(&q, now + 970000, 40000, 5000, &expired) == (int)s0);
    CHECK(lwc_jit_due(&q, now + 997000, 40000, 5000, &expired) == -1 && expired == (int)s0);
    lwc_jit_release(&q, s0);
    CHECK(lwc_jit_due(&q, now + 1980000, 40000, 5000, &expired) == (int)s1);

    /* a sent frame is never due again but still blocks overlaps until it ends */
    lwc_jit_t g;
    lwc_jit_init(&g, 20000, 10000000, 40000);
    size_t sa;
    CHECK(lwc_jit_admit(&g, now, now + 100000, 1500000, 7, &sa) == LWC_JIT_OK);
    lwc_jit_sent(&g, sa);
    CHECK(lwc_jit_due(&g, now + 90000, 40000, 5000, &expired) == -1 && expired == -1);
    CHECK(lwc_jit_admit(&g, now + 90000, now + 600000, 50000, 8, NULL) == LWC_JIT_COLLISION);
    CHECK(lwc_jit_admit(&g, now + 90000, now + 1620000, 50000, 8, NULL) == LWC_JIT_COLLISION); /* inside guard */
    CHECK(lwc_jit_ended(&g, now + 1600000, 0) == -1);
    CHECK(lwc_jit_ended(&g, now + 1600001, 0) == (int)sa);
    lwc_jit_release(&g, sa);
    CHECK(lwc_jit_admit(&g, now + 1600001, now + 1700000, 50000, 9, NULL) == LWC_JIT_OK);

    /* counter wrap: now near UINT32_MAX, start after the wrap */
    lwc_jit_t w;
    lwc_jit_init(&w, 20000, 10000000, 1000);
    const uint32_t n2 = 0xFFFF0000U;
    CHECK(lwc_jit_admit(&w, n2, n2 + 1000000U, 50000, 3, &s0) == LWC_JIT_OK);
    CHECK(lwc_jit_due(&w, n2 + 970000U, 40000, 5000, &expired) == (int)s0);

    /* full */
    lwc_jit_t f;
    lwc_jit_init(&f, 0, 100000000, 0);
    for (uint32_t i = 0; i < LWC_JIT_SLOTS; i++) {
        CHECK(lwc_jit_admit(&f, 0, 1000 + i * 100000, 1000, i, NULL) == LWC_JIT_OK);
    }
    CHECK(lwc_jit_admit(&f, 0, 50000000, 1000, 99, NULL) == LWC_JIT_FULL);
}

static void test_dutycycle(void)
{
    lwc_dutycycle_t dc;
    lwc_dc_init(&dc, 0);
    CHECK(lwc_dc_band(868100000U) == 2 && lwc_dc_permille(2) == 10);
    CHECK(lwc_dc_band(869525000U) == 4 && lwc_dc_permille(4) == 100);
    CHECK(lwc_dc_band(868650000U) == -1); /* gap between g1 and g2 */
    CHECK(lwc_dc_band(870000000U) == -1);
    /* g1 1 %: 36 s per hour */
    CHECK(lwc_dc_take(&dc, 0, 868100000U, 36000000U));
    CHECK(!lwc_dc_take(&dc, 0, 868100000U, 1U));
    /* 100 s later: 1 s refilled */
    CHECK(lwc_dc_take(&dc, 100000000LL, 868100000U, 1000000U));
    CHECK(!lwc_dc_take(&dc, 100000000LL, 868100000U, 1U));
    /* other band unaffected */
    CHECK(lwc_dc_take(&dc, 100000000LL, 869525000U, 1000000U));
    CHECK(!lwc_dc_take(&dc, 0, 870500000U, 1U));
}

static void test_airtime(void)
{
    /* Reference values: Semtech LoRa calculator / TTN airtime calculator. */
    CHECK(lwc_lora_airtime_us(9, 125, 5, 8, 10, true, false) == 144384U);  /* test frame */
    CHECK(lwc_lora_airtime_us(7, 125, 5, 8, 13, true, false) == 46336U);
    CHECK(lwc_lora_airtime_us(12, 125, 5, 8, 23, true, false) == 1482752U); /* LDRO */
    CHECK(lwc_lora_airtime_us(7, 125, 5, 8, 13, false, false) == 41216U);   /* downlink, no CRC */
    CHECK(lwc_lora_airtime_us(13, 125, 5, 8, 10, true, false) == 0U);
    CHECK(lwc_lora_airtime_us(7, 200, 5, 8, 10, true, false) == 0U);
}

int main(void)
{
    test_airtime();
    test_jit();
    test_dutycycle();
    if (failures != 0) {
        fprintf(stderr, "test_txpk: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_txpk: all passed\n");
    return 0;
}
