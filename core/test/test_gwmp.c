#include "lwc/gwmp.h"

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

static void test_base64(void)
{
    char out[16];
    /* RFC 4648 test vectors */
    CHECK(lwc_base64_encode((const uint8_t *)"", 0, out, sizeof out) == 0 && out[0] == '\0');
    CHECK(lwc_base64_encode((const uint8_t *)"f", 1, out, sizeof out) == 4 && !strcmp(out, "Zg=="));
    CHECK(lwc_base64_encode((const uint8_t *)"fo", 2, out, sizeof out) == 4 && !strcmp(out, "Zm8="));
    CHECK(lwc_base64_encode((const uint8_t *)"foo", 3, out, sizeof out) == 4 && !strcmp(out, "Zm9v"));
    CHECK(lwc_base64_encode((const uint8_t *)"foobar", 6, out, sizeof out) == 8 &&
          !strcmp(out, "Zm9vYmFy"));
    CHECK(lwc_base64_encode((const uint8_t *)"foobar", 6, out, 8) == 0); /* no room for NUL */
}

static void test_time(void)
{
    char t[32];
    CHECK(lwc_time_iso8601_us(0, t, sizeof t) == 0 && !strcmp(t, "1970-01-01T00:00:00.000000Z"));
    /* 2026-11-21T23:30:00.123456Z = 1795303800 s */
    CHECK(lwc_time_iso8601_us(1795303800123456LL, t, sizeof t) == 0 &&
          !strcmp(t, "2026-11-21T23:30:00.123456Z"));
    /* leap day 2024-02-29T12:00:00Z = 1709208000 */
    CHECK(lwc_time_stat(1709208000LL, t, sizeof t) == 0 && !strcmp(t, "2024-02-29 12:00:00 GMT"));
    CHECK(lwc_time_iso8601_us(-1, t, sizeof t) == -1);
    CHECK(lwc_time_iso8601_us(0, t, 27) == -1);
}

static void test_header_and_eui(void)
{
    const uint8_t mac[6] = {0xe8, 0xf6, 0x0a, 0xe8, 0x96, 0x5a};
    uint8_t eui[8];
    lwc_eui64_from_mac(mac, eui);
    const uint8_t want[8] = {0xe8, 0xf6, 0x0a, 0xff, 0xfe, 0xe8, 0x96, 0x5a};
    CHECK(memcmp(eui, want, 8) == 0);

    uint8_t h[12];
    CHECK(lwc_gwmp_header(h, sizeof h, LWC_GWMP_PULL_DATA, 0xBEEF, eui) == 12);
    CHECK(h[0] == 2 && h[1] == 0xBE && h[2] == 0xEF && h[3] == 2 && memcmp(&h[4], eui, 8) == 0);
    CHECK(lwc_gwmp_header(h, 11, LWC_GWMP_PULL_DATA, 1, eui) == 0);
    CHECK(lwc_gwmp_header(h, 4, LWC_GWMP_TX_ACK, 1, NULL) == 4);
}

static void test_rxpk(void)
{
    const uint8_t pl[] = {0x40, 0x04, 0x03, 0x02, 0x01};
    lwc_rxpk_t p = {
        .time_unix_us = 1795303800123456LL, .tmst = 3512348611U, .freq_hz = 868100000U,
        .chan = 0, .rfch = 1, .stat = 1, .sf = 7, .bw_khz = 125, .cr_denom = 5,
        .rssi = -35, .lsnr = 5.1f, .size = sizeof pl, .payload = pl,
    };
    char out[512];
    size_t len = 0;
    CHECK(lwc_gwmp_rxpk_json(out, sizeof out, &p, 1, &len) == LWC_GWMP_OK);
    const char *want =
        "{\"rxpk\":[{\"time\":\"2026-11-21T23:30:00.123456Z\",\"tmst\":3512348611,\"chan\":0,"
        "\"rfch\":1,\"freq\":868.100000,\"stat\":1,\"modu\":\"LORA\",\"datr\":\"SF7BW125\","
        "\"codr\":\"4/5\",\"rssi\":-35,\"lsnr\":5.1,\"size\":5,\"data\":\"QAQDAgE=\"}]}";
    CHECK(strcmp(out, want) == 0);
    CHECK(len == strlen(want));

    p.time_unix_us = -1; /* no clock: no "time" field */
    CHECK(lwc_gwmp_rxpk_json(out, sizeof out, &p, 1, &len) == LWC_GWMP_OK);
    CHECK(strstr(out, "\"time\"") == NULL);

    CHECK(lwc_gwmp_rxpk_json(out, 40, &p, 1, &len) == LWC_GWMP_ERR_SPACE);
    p.sf = 13;
    CHECK(lwc_gwmp_rxpk_json(out, sizeof out, &p, 1, &len) == LWC_GWMP_ERR_FIELD);
}

static void test_stat(void)
{
    lwc_stat_t st = {.time_unix_s = 1795303800, .rxnb = 3, .rxok = 2, .rxfw = 2,
                     .ackr_pct = 100, .dwnb = 0, .txnb = 0};
    char out[256];
    size_t len;
    CHECK(lwc_gwmp_stat_json(out, sizeof out, &st, &len) == LWC_GWMP_OK);
    CHECK(strcmp(out, "{\"stat\":{\"time\":\"2026-11-21 23:30:00 GMT\",\"rxnb\":3,\"rxok\":2,"
                      "\"rxfw\":2,\"ackr\":100.0,\"dwnb\":0,\"txnb\":0}}") == 0);
}

static void test_parse_down(void)
{
    lwc_gwmp_down_t d;
    const uint8_t ack[] = {2, 0x12, 0x34, 1};
    CHECK(lwc_gwmp_parse_down(ack, sizeof ack, &d) == LWC_GWMP_OK);
    CHECK(d.type == LWC_GWMP_PUSH_ACK && d.token == 0x1234 && d.json == NULL);

    const uint8_t resp[] = {2, 0, 7, 3, '{', '}'};
    CHECK(lwc_gwmp_parse_down(resp, sizeof resp, &d) == LWC_GWMP_OK);
    CHECK(d.type == LWC_GWMP_PULL_RESP && d.json_len == 2 && d.json[0] == '{');

    const uint8_t bad_ver[] = {1, 0, 0, 1};
    CHECK(lwc_gwmp_parse_down(bad_ver, sizeof bad_ver, &d) == LWC_GWMP_ERR_VERSION);
    const uint8_t up_type[] = {2, 0, 0, 0};
    CHECK(lwc_gwmp_parse_down(up_type, sizeof up_type, &d) == LWC_GWMP_ERR_TYPE);
    CHECK(lwc_gwmp_parse_down(ack, 3, &d) == LWC_GWMP_ERR_LENGTH);
    const uint8_t empty_resp[] = {2, 0, 0, 3};
    CHECK(lwc_gwmp_parse_down(empty_resp, sizeof empty_resp, &d) == LWC_GWMP_ERR_LENGTH);
}

int main(void)
{
    test_base64();
    test_time();
    test_header_and_eui();
    test_rxpk();
    test_stat();
    test_parse_down();
    if (failures != 0) {
        fprintf(stderr, "test_gwmp: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gwmp: all passed\n");
    return 0;
}
