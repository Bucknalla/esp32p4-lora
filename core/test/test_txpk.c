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

static lwc_txpk_err_t parse(const char *s, lwc_txpk_t *t)
{
    return lwc_txpk_parse(s, strlen(s), t);
}

static void test_ttn_join_accept(void)
{
    /* Shape of a TTN v3 RX1 downlink (values illustrative). */
    const char *j = "{\"txpk\":{\"imme\":false,\"rfch\":0,\"powe\":16.15,\"ant\":0,\"brd\":0,"
                    "\"tmst\":4028567890,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":\"SF7BW125\","
                    "\"codr\":\"4/5\",\"ipol\":true,\"size\":5,\"data\":\"IAECAwQ=\"}}";
    lwc_txpk_t t;
    CHECK(parse(j, &t) == LWC_TXPK_OK);
    CHECK(t.mode == LWC_TXPK_TIMED && t.tmst == 4028567890U);
    CHECK(t.freq_hz == 868100000U && t.rfch == 0 && t.powe == 16);
    CHECK(t.sf == 7 && t.bw_khz == 125 && t.cr_denom == 5 && t.ipol && !t.ncrc);
    CHECK(t.size == 5 && t.payload[0] == 0x20 && t.payload[4] == 0x04);
}

static void test_rx2_and_order(void)
{
    /* Keys in another order, RX2 frequency with 3 decimals, whitespace. */
    const char *j = "{ \"txpk\" : { \"data\":\"QAE=\", \"datr\":\"SF9BW125\", \"freq\":869.525,"
                    " \"codr\":\"4/5\", \"modu\":\"LORA\", \"tmst\":100, \"powe\":27 } }";
    lwc_txpk_t t;
    CHECK(parse(j, &t) == LWC_TXPK_OK);
    CHECK(t.freq_hz == 869525000U && t.sf == 9 && t.powe == 27 && t.size == 2);
}

static void test_modes_and_rejects(void)
{
    lwc_txpk_t t;
    CHECK(parse("{\"txpk\":{\"imme\":true,\"freq\":869.525,\"modu\":\"LORA\",\"datr\":\"SF12BW125\","
                "\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_OK);
    CHECK(t.mode == LWC_TXPK_IMMEDIATE);
    CHECK(parse("{\"txpk\":{\"tmms\":1234,\"freq\":869.525,\"modu\":\"LORA\",\"datr\":\"SF12BW125\","
                "\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_OK);
    CHECK(t.mode == LWC_TXPK_GPS);
    /* FSK */
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"freq\":868.8,\"modu\":\"FSK\",\"datr\":50000,"
                "\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_ERR_MODU);
    /* size mismatch */
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":\"SF7BW125\","
                "\"codr\":\"4/5\",\"size\":3,\"data\":\"AA==\"}}", &t) == LWC_TXPK_ERR_FIELD);
    /* missing data */
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":\"SF7BW125\","
                "\"codr\":\"4/5\"}}", &t) == LWC_TXPK_ERR_FIELD);
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":\"SF13BW125\","
                "\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_ERR_FIELD);
    CHECK(parse("{\"txpk\":{\"tmst\":4294967296,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":"
                "\"SF7BW125\",\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_ERR_FIELD);
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"freq\":868.1000001,\"modu\":\"LORA\",\"datr\":"
                "\"SF7BW125\",\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_ERR_FIELD);
    CHECK(parse("{\"rxpk\":[]}", &t) == LWC_TXPK_ERR_JSON);
    CHECK(parse("{\"txpk\":{\"tmst\":1}", &t) != LWC_TXPK_OK); /* truncated */
    CHECK(parse("{\"txpk\":{}} trailing", &t) != LWC_TXPK_OK);
    CHECK(lwc_txpk_parse(NULL, 0, &t) == LWC_TXPK_ERR_ARG);
    /* fractional negative power floors */
    CHECK(parse("{\"txpk\":{\"tmst\":1,\"powe\":-2.5,\"freq\":868.1,\"modu\":\"LORA\",\"datr\":"
                "\"SF7BW125\",\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_OK && t.powe == -3);
    /* unknown nested keys are skipped */
    CHECK(parse("{\"txpk\":{\"x\":{\"y\":[1,2,{\"z\":null}]},\"tmst\":1,\"freq\":868.1,\"modu\":"
                "\"LORA\",\"datr\":\"SF7BW125\",\"codr\":\"4/5\",\"data\":\"AA==\"}}", &t) == LWC_TXPK_OK);
}

static void test_base64_decode(void)
{
    uint8_t b[8];
    CHECK(lwc_base64_decode("Zm9vYmFy", 8, b, sizeof b) == 6 && memcmp(b, "foobar", 6) == 0);
    CHECK(lwc_base64_decode("Zg==", 4, b, sizeof b) == 1 && b[0] == 'f');
    CHECK(lwc_base64_decode("Zg", 2, b, sizeof b) == 1);
    CHECK(lwc_base64_decode("Z", 1, b, sizeof b) == -1);
    CHECK(lwc_base64_decode("Zm9v!", 5, b, sizeof b) == -1);
    CHECK(lwc_base64_decode("Zm9vYmFy", 8, b, 5) == -1);
}

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
    test_ttn_join_accept();
    test_rx2_and_order();
    test_modes_and_rejects();
    test_base64_decode();
    test_jit();
    test_dutycycle();
    if (failures != 0) {
        fprintf(stderr, "test_txpk: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_txpk: all passed\n");
    return 0;
}
