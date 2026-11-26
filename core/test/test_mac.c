/* MAC commands and GPS time. */
#include "lwc/mac.h"

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

static void parse(void)
{
    lwc_mac_up_t m;
    /* LinkCheckReq, DevStatusAns (battery 200, margin -5), DeviceTimeReq, LinkADRAns x2 */
    static const uint8_t a[] = {0x02, 0x06, 200, 0x3B, 0x0D, 0x03, 0x07, 0x03, 0x05};
    CHECK(lwc_mac_parse_up(a, sizeof a, &m) == 5U);
    CHECK(m.link_check_req && m.device_time_req && !m.stopped);
    CHECK(m.dev_status_ans && m.battery == 200 && m.margin == -5);
    CHECK(m.link_adr_ans == 2 && m.link_adr_status == 0x05); /* DR refused once */
    static const uint8_t pos[] = {0x06, 0xFF, 0x1F};
    CHECK(lwc_mac_parse_up(pos, sizeof pos, &m) == 1U && m.battery == 255 && m.margin == 31);
    /* answers counted only */
    static const uint8_t b[] = {0x04, 0x05, 0x07, 0x07, 0x03, 0x08, 0x09, 0x0A, 0x03};
    CHECK(lwc_mac_parse_up(b, sizeof b, &m) == 6U && m.other == 5 && !m.stopped && m.link_adr_ans == 0);
    CHECK(m.tx_param_setup_ans); /* TxParamSetupAns: no payload */
    CHECK(m.link_adr_status == 0);
    /* unknown CID: stop, keep what came before */
    static const uint8_t c[] = {0x02, 0x01, 0x0D};
    CHECK(lwc_mac_parse_up(c, sizeof c, &m) == 1U && m.link_check_req && !m.device_time_req && m.stopped);
    static const uint8_t prop[] = {0x80, 0x02};
    CHECK(lwc_mac_parse_up(prop, sizeof prop, &m) == 0U && m.stopped && !m.link_check_req);
    /* cut short */
    static const uint8_t d[] = {0x02, 0x06, 0x10};
    CHECK(lwc_mac_parse_up(d, sizeof d, &m) == 1U && m.stopped && !m.dev_status_ans);
    CHECK(lwc_mac_parse_up(NULL, 4, &m) == 0U && lwc_mac_parse_up(a, 0, &m) == 0U && !m.stopped);
    CHECK(lwc_mac_parse_up(a, sizeof a, NULL) == 0U);
}

static void build(void)
{
    uint8_t buf[15];
    size_t n = 0;
    CHECK(lwc_mac_link_check_ans(buf, sizeof buf, &n, 7, 1) && n == 3 && buf[0] == 0x02 && buf[1] == 7 && buf[2] == 1);
    CHECK(lwc_mac_device_time_ans(buf, sizeof buf, &n, 0x12345678U, 0x80) && n == 9);
    CHECK(buf[3] == 0x0D && buf[4] == 0x78 && buf[5] == 0x56 && buf[6] == 0x34 && buf[7] == 0x12 && buf[8] == 0x80);
    CHECK(lwc_mac_link_adr_req(buf, sizeof buf, &n, 5, 2, 0x00FF, 0, 1) && n == 14);
    CHECK(buf[9] == 0x03 && buf[10] == 0x52 && buf[11] == 0xFF && buf[12] == 0x00 && buf[13] == 0x01);
    CHECK(lwc_mac_dev_status_req(buf, sizeof buf, &n) && n == 15 && buf[14] == 0x06);
    CHECK(!lwc_mac_dev_status_req(buf, sizeof buf, &n) && n == 15); /* full */
    n = 0;
    CHECK(!lwc_mac_link_adr_req(buf, sizeof buf, &n, 16, 0, 0, 0, 1) && n == 0);
    CHECK(lwc_mac_link_check_ans(buf, sizeof buf, &n, 255, 1) && buf[1] == 254); /* 255 reserved */
    /* TxParamSetupReq: bit 5 downlink dwell, bit 4 uplink dwell, MaxEIRP index */
    n = 0;
    CHECK(lwc_mac_tx_param_setup_req(buf, sizeof buf, &n, false, true, 5) && n == 2 && buf[0] == 0x09 && buf[1] == 0x15);
    CHECK(lwc_mac_tx_param_setup_req(buf, sizeof buf, &n, true, false, 13) && n == 4 && buf[3] == 0x2D);
    CHECK(!lwc_mac_tx_param_setup_req(buf, sizeof buf, &n, false, false, 16) && n == 4);
    CHECK(lwc_mac_max_eirp_dbm(0) == 8 && lwc_mac_max_eirp_dbm(5) == 16 && lwc_mac_max_eirp_dbm(13) == 30 &&
          lwc_mac_max_eirp_dbm(15) == 36 && lwc_mac_max_eirp_dbm(16) == -1);
    CHECK(lwc_mac_max_eirp_idx(16) == 5 && lwc_mac_max_eirp_idx(30) == 13 && lwc_mac_max_eirp_idx(17) == 5 &&
          lwc_mac_max_eirp_idx(7) == -1 && lwc_mac_max_eirp_idx(40) == 15);
}

static void gps(void)
{
    uint32_t s = 0;
    uint8_t f = 0;
    /* GPS week 2048 began 2019-04-06T23:59:42Z (the 2019 week rollover) */
    CHECK(lwc_gps_from_unix_us(1554595182LL * 1000000LL, &s, &f) && s == 2048U * 604800U && f == 0);
    CHECK(lwc_gps_from_unix_us(1554595182LL * 1000000LL + 500000, &s, &f) && f == 128);
    CHECK(lwc_gps_from_unix_us(1554595182LL * 1000000LL + 999999, &s, &f) && f == 255);
    CHECK(!lwc_gps_from_unix_us(-1, &s, &f));
    CHECK(!lwc_gps_from_unix_us(1000000LL, &s, &f)); /* clock not set */
}

int main(void)
{
    parse();
    build();
    gps();
    if (failures != 0) {
        fprintf(stderr, "test_mac: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_mac: all passed\n");
    return 0;
}
