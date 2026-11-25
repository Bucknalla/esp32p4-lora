/* LoRa Basics Station LNS messages (ADR-0016). Expected strings follow the
 * Semtech protocol doc and The Things Stack's lbslns tests (v3.37). */
#include "lwc/lbs.h"

#include <stdio.h>
#include <string.h>

#include "lbs_rebuild.h"

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

#define STR_IS(buf, n, want) ((n) == strlen(want) && memcmp((buf), (want), (n)) == 0)

static char out[1400];

static void drs(void)
{
    CHECK(lwc_lbs_dr(EU, 12, 125) == 0 && lwc_lbs_dr(EU, 7, 125) == 5 && lwc_lbs_dr(EU, 7, 250) == 6);
    CHECK(lwc_lbs_dr(EU, 8, 250) == -1 && lwc_lbs_dr(EU, 6, 125) == -1 && lwc_lbs_dr(EU, 7, 500) == -1);
    uint8_t sf;
    uint16_t bw;
    CHECK(lwc_lbs_dr_params(EU, 0, &sf, &bw) && sf == 12 && bw == 125);
    CHECK(lwc_lbs_dr_params(EU, 6, &sf, &bw) && sf == 7 && bw == 250);
    CHECK(!lwc_lbs_dr_params(EU, 7, &sf, &bw));
    /* ADR-0024: the gateway's region. IN865 has no DR6 (SF7 at 250 kHz). */
    const lwc_region_t *in = lwc_region(LWC_REGION_IN865);
    CHECK(lwc_lbs_dr(in, 7, 125) == 5 && lwc_lbs_dr(in, 7, 250) == -1 && !lwc_lbs_dr_params(in, 6, &sf, &bw));
    CHECK(lwc_lbs_dr(NULL, 7, 125) == -1 && !lwc_lbs_dr_params(NULL, 0, &sf, &bw));
}

static void xtime(void)
{
    lwc_lbs_clock_t c;
    lwc_lbs_clock_init(&c, 0x11);
    const int64_t s = (int64_t)0x11 << 48;
    CHECK(lwc_lbs_xtime(&c, 1000) == s + 1000);
    CHECK(lwc_lbs_xtime(&c, 0x7FFFFF00U) == s + 0x7FFFFF00LL); /* fed within 2^31 us */
    CHECK(lwc_lbs_xtime(&c, 0xFFFFFE00U) == s + 0xFFFFFE00LL);
    CHECK(lwc_lbs_xtime(&c, 0xFFFFFF00U) == s + 0xFFFFFF00LL);
    /* the counter wraps: the 48-bit time goes on */
    CHECK(lwc_lbs_xtime(&c, 0x100) == s + 0x100000100LL);
    /* an uplink from just before the last sample maps behind it, without moving the clock */
    CHECK(lwc_lbs_xtime(&c, 0xFFFFFFF0U) == s + 0xFFFFFFF0LL);
    CHECK(lwc_lbs_xtime(&c, 0x200) == s + 0x100000200LL);
    lwc_lbs_clock_init(&c, 0);
    CHECK(c.sess == 1); /* never 0 */
}

static void version(void)
{
    size_t n = 0;
    CHECK(lwc_lbs_version(out, sizeof out, "lwc-gw", "v0.3.0", "v0.3.0", "esp32p4-sx1301", &n) == LWC_LBS_OK);
    CHECK(STR_IS(out, n, "{\"msgtype\":\"version\",\"station\":\"lwc-gw\",\"firmware\":\"v0.3.0\",\"package\":\"v0.3.0\","
                         "\"model\":\"esp32p4-sx1301\",\"protocol\":2,\"features\":\"\"}"));
    CHECK(lwc_lbs_version(out, sizeof out, "a\"b", "x", "x", "x", &n) == LWC_LBS_ERR_ARG);
    CHECK(lwc_lbs_version(out, 20, "a", "b", "c", "d", &n) == LWC_LBS_ERR_SPACE);
}

static void uplinks(void)
{
    const lwc_lbs_rx_t rx = {.sf = 11, .bw_khz = 125, .freq_hz = 868300000, .xtime = 12666373963464220LL,
                             .rssi = -89.0f, .snr = 9.25f, .unix_us = 1548059982000000LL};
    uint8_t back[255];
    size_t n = 0;

    /* jreq: the values of TTS's upstream test (JoinEui 22.., DevEui 11.., DevNonce 18000, MIC 12345678) */
    static const uint8_t jr[23] = {0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x11, 0x11, 0x11,
                                   0x11, 0x11, 0x11, 0x11, 0x11, 0x50, 0x46, 0x4E, 0x61, 0xBC, 0x00};
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, jr, sizeof jr, &rx, &n) == LWC_LBS_OK);
    CHECK(STR_IS(out, n,
                 "{\"msgtype\":\"jreq\",\"MHdr\":0,\"JoinEui\":\"22-22-22-22-22-22-22-22\",\"DevEui\":\"11-11-11-11-11-11-11-11\","
                 "\"DevNonce\":18000,\"MIC\":12345678,\"DR\":1,\"Freq\":868300000,\"upinfo\":{\"rctx\":0,"
                 "\"xtime\":12666373963464220,\"gpstime\":0,\"rssi\":-89,\"snr\":9.25,\"rxtime\":1548059982.000000}}"));
    CHECK(lbs_rebuild(out, n, back, sizeof back) == sizeof jr && memcmp(back, jr, sizeof jr) == 0);

    /* updf: DevAddr and MIC with the top bit set go out negative (signed int32) */
    static const uint8_t up[] = {0x80, 0x04, 0x03, 0x02, 0x81, 0xA2, 0x34, 0x12, 0x03, 0x07,
                                 0x0A, 0xAA, 0xBB, 0x01, 0x02, 0x03, 0xFE};
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, up, sizeof up, &rx, &n) == LWC_LBS_OK);
    CHECK(STR_IS(out, n,
                 "{\"msgtype\":\"updf\",\"MHdr\":128,\"DevAddr\":-2130574588,\"FCtrl\":162,\"FCnt\":4660,"
                 "\"FOpts\":\"0307\",\"FPort\":10,\"FRMPayload\":\"AABB\",\"MIC\":-33357311,\"DR\":1,\"Freq\":868300000,"
                 "\"upinfo\":{\"rctx\":0,\"xtime\":12666373963464220,\"gpstime\":0,\"rssi\":-89,\"snr\":9.25,"
                 "\"rxtime\":1548059982.000000}}"));
    CHECK(lbs_rebuild(out, n, back, sizeof back) == sizeof up && memcmp(back, up, sizeof up) == 0);

    /* no port: FPort -1, empty payload; no clock: rxtime 0 */
    static const uint8_t noport[] = {0x40, 0x04, 0x03, 0x02, 0x01, 0x00, 0x05, 0x00, 0x11, 0x22, 0x33, 0x44};
    lwc_lbs_rx_t rx2 = rx;
    rx2.unix_us = -1;
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, noport, sizeof noport, &rx2, &n) == LWC_LBS_OK);
    CHECK(strstr(out, "\"FOpts\":\"\",\"FPort\":-1,\"FRMPayload\":\"\",\"MIC\":1144201745") != NULL);
    CHECK(strstr(out, "\"rxtime\":0}}") != NULL);
    CHECK(lbs_rebuild(out, n, back, sizeof back) == sizeof noport && memcmp(back, noport, sizeof noport) == 0);

    /* proprietary: the whole frame */
    static const uint8_t prop[] = {0xE0, 0x01, 0x02};
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, prop, sizeof prop, &rx, &n) == LWC_LBS_OK);
    CHECK(strncmp(out, "{\"msgtype\":\"propdf\",\"FRMPayload\":\"E00102\",\"DR\":1,", 49) == 0);
    CHECK(lbs_rebuild(out, n, back, sizeof back) == sizeof prop && memcmp(back, prop, sizeof prop) == 0);

    /* refused: downlinks, short frames, no EU868 data rate, no room */
    static const uint8_t down[] = {0x60, 0x04, 0x03, 0x02, 0x01, 0x00, 0x05, 0x00, 0x11, 0x22, 0x33, 0x44};
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, down, sizeof down, &rx, &n) == LWC_LBS_ERR_FRAME);
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, jr, 22, &rx, &n) == LWC_LBS_ERR_FRAME);
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, up, 11, &rx, &n) == LWC_LBS_ERR_FRAME);
    rx2.bw_khz = 500;
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, up, sizeof up, &rx2, &n) == LWC_LBS_ERR_DR);
    CHECK(lwc_lbs_uplink(out, 100, EU, up, sizeof up, &rx, &n) == LWC_LBS_ERR_SPACE);
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, NULL, 0, &rx, &n) == LWC_LBS_ERR_ARG);
    static uint8_t big[256];
    big[0] = 0xE0;
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, big, 255, &rx, &n) == LWC_LBS_OK);
    CHECK(lwc_lbs_uplink(out, sizeof out, EU, big, 256, &rx, &n) == LWC_LBS_ERR_FRAME);
}

static void dntxed(void)
{
    size_t n = 0;
    CHECK(lwc_lbs_dntxed(out, sizeof out, 9007199254740993LL, "00-00-00-00-00-00-00-01", 0,
                         ((int64_t)0x11 << 48) | 1553300787, 1552906698123456LL, &n) == LWC_LBS_OK);
    CHECK(STR_IS(out, n,
                 "{\"msgtype\":\"dntxed\",\"diid\":9007199254740993,\"DevEui\":\"00-00-00-00-00-00-00-01\",\"rctx\":0,"
                 "\"xtime\":4785076157381939,\"txtime\":1552906698.123456,\"gpstime\":0}"));
    CHECK(lwc_lbs_dntxed(out, sizeof out, 1, "a\"", 0, 1, 0, &n) == LWC_LBS_ERR_ARG);
}

static void msgtypes(void)
{
    lwc_lbs_msg_t t;
    static const char rc[] = "{\"msgtype\":\"router_config\"}";
    CHECK(lwc_lbs_msgtype(rc, strlen(rc), &t) == LWC_LBS_OK && t == LWC_LBS_MSG_ROUTER_CONFIG);
    static const char dn[] = " { \"DevEui\": \"x\", \"nested\": {\"msgtype\":\"no\"}, \"msgtype\" : \"dnmsg\" }";
    CHECK(lwc_lbs_msgtype(dn, strlen(dn), &t) == LWC_LBS_OK && t == LWC_LBS_MSG_DNMSG);
    static const char ts[] = "{\"msgtype\":\"timesync\",\"txtime\":1,\"gpstime\":2}";
    CHECK(lwc_lbs_msgtype(ts, strlen(ts), &t) == LWC_LBS_OK && t == LWC_LBS_MSG_OTHER);
    static const char none[] = "{\"a\":1}";
    CHECK(lwc_lbs_msgtype(none, strlen(none), &t) == LWC_LBS_ERR_FIELD);
    CHECK(lwc_lbs_msgtype("[1]", 3, &t) == LWC_LBS_ERR_JSON);
    CHECK(lwc_lbs_msgtype("{\"msgtype\":", 11, &t) == LWC_LBS_ERR_FIELD);
}

static void dnmsgs(void)
{
    lwc_lbs_dnmsg_t d;
    lwc_txpk_t a, b;
    /* class A as TTS sends it: RX1 only, xtime one second before the window, RxDelay 1 */
    static const char tts[] =
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"00-00-00-00-00-00-00-01\",\"dC\":0,\"diid\":1,"
        "\"pdu\":\"596d7868616d74686332356b4a334d3d3d\",\"priority\":25,\"rctx\":2,\"MuxTime\":1554300787.123,"
        "\"RxDelay\":1,\"RX1DR\":2,\"RX1Freq\":868500000,\"xtime\":4785076156381939}";
    CHECK(lwc_lbs_parse_dnmsg(tts, strlen(tts), &d) == LWC_LBS_OK);
    CHECK(strcmp(d.dev_eui, "00-00-00-00-00-00-00-01") == 0 && d.diid == 1 && d.dclass == 0 && d.rctx == 2);
    CHECK(d.len == 17 && d.pdu[0] == 0x59 && d.pdu[16] == 0x3d && d.priority == 25 && d.rx_delay == 1);
    CHECK(d.rx1 && d.rx1_dr == 2 && d.rx1_freq == 868500000 && !d.rx2 && d.xtime == 4785076156381939LL);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_OK);
    CHECK(a.mode == LWC_TXPK_TIMED && a.tmst == 1553300787U && a.freq_hz == 868500000 && a.sf == 10 && a.bw_khz == 125);
    CHECK(a.powe == 16 && a.ipol && a.ncrc && a.cr_denom == 5 && a.size == 17 && a.payload[0] == 0x59);
    CHECK(b.size == 0); /* no RX2 given: no second window */
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x12, &a, &b) == LWC_LBS_ERR_SESSION);

    /* RX1 and RX2 (Station's form); RX2 at 869.525 MHz with the higher power; diid above 2^53 exact */
    static const char both[] =
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"0011:2233:4455:6677\",\"dC\":0,\"diid\":9007199254740993,\"pdu\":\"60AB\","
        "\"RxDelay\":5,\"RX1DR\":5,\"RX1Freq\":868100000,\"RX2DR\":0,\"RX2Freq\":869525000,"
        "\"priority\":0,\"xtime\":4785074604081153,\"rctx\":0}";
    CHECK(lwc_lbs_parse_dnmsg(both, strlen(both), &d) == LWC_LBS_OK && d.diid == 9007199254740993LL);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_OK);
    CHECK(a.tmst == 5000001U && a.sf == 7 && a.freq_hz == 868100000 && a.powe == 16);
    CHECK(b.size == 2 && b.tmst == 6000001U && b.sf == 12 && b.freq_hz == 869525000 && b.powe == 27);

    /* RX2 only: one window, one second after RX1 would be */
    static const char rx2[] =
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX2DR\":3,\"RX2Freq\":869525000,"
        "\"xtime\":4785074604081153}";
    CHECK(lwc_lbs_parse_dnmsg(rx2, strlen(rx2), &d) == LWC_LBS_OK && d.rx_delay == 1);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_OK && a.tmst == 2000001U && a.sf == 9 && b.size == 0);

    /* RxDelay 0 reads as 1; the "DR"/"Freq" form sends at xtime itself */
    static const char d0[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RxDelay\":0,"
                             "\"RX1DR\":5,\"RX1Freq\":868100000,\"xtime\":4785074604081153}";
    CHECK(lwc_lbs_parse_dnmsg(d0, strlen(d0), &d) == LWC_LBS_OK && d.rx_delay == 1);
    static const char df[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"DR\":5,"
                             "\"Freq\":868100000,\"xtime\":4785074604081153}";
    CHECK(lwc_lbs_parse_dnmsg(df, strlen(df), &d) == LWC_LBS_OK && d.rx_delay == 0);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_OK && a.tmst == 1U);

    /* not supported here: class B, GPS time */
    static const char cb[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":1,\"diid\":3,\"pdu\":\"60\",\"DR\":3,"
                             "\"Freq\":869525000,\"gpstime\":1234}";
    CHECK(lwc_lbs_parse_dnmsg(cb, strlen(cb), &d) == LWC_LBS_ERR_UNSUPPORTED);

    /* class C "immediately" as ChirpStack's bridge sends it: dC 2, RX2 only, no
     * xtime (or the last uplink's): at the next free time on RX2 */
    static const char cc[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01-01-01-01-01-01-01-01\",\"dC\":2,\"diid\":3,"
                             "\"pdu\":\"60AB\",\"RX2DR\":3,\"RX2Freq\":869525000,\"priority\":1,\"MuxTime\":1554300787.1}";
    CHECK(lwc_lbs_parse_dnmsg(cc, strlen(cc), &d) == LWC_LBS_OK && d.asap && d.dclass == 2U && d.xtime == 0);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_ERR_UNSUPPORTED); /* not timed by xtime */
    CHECK(lwc_lbs_dn_txpk_now(&d, EU, 5000000U, &a, &b) == LWC_LBS_OK);
    CHECK(a.mode == LWC_TXPK_TIMED && a.tmst == 5000000U + LWC_LBS_ASAP_LEAD_US && a.freq_hz == 869525000U && a.sf == 9 &&
          a.powe == 27 && a.size == 2 && a.payload[1] == 0xAB);
    CHECK(b.tmst == a.tmst + LWC_LBS_ASAP_RETRY_US && b.freq_hz == a.freq_hz && b.sf == a.sf && b.size == 2);
    CHECK(lwc_lbs_dn_txpk_now(&d, EU, 0xFFFFFF00U, &a, &b) == LWC_LBS_OK && a.tmst == 0xFFFFFF00U + LWC_LBS_ASAP_LEAD_US); /* wraps */
    /* with the last uplink's xtime, even from an earlier session: still now */
    static const char ccx[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":2,\"diid\":4,\"pdu\":\"60\",\"RX2DR\":0,"
                              "\"RX2Freq\":869525000,\"xtime\":4785074604081153,\"rctx\":0}";
    CHECK(lwc_lbs_parse_dnmsg(ccx, strlen(ccx), &d) == LWC_LBS_OK && d.asap);
    CHECK(lwc_lbs_dn_txpk_now(&d, EU, 100U, &a, &b) == LWC_LBS_OK && a.tmst == 100U + LWC_LBS_ASAP_LEAD_US && a.sf == 12);
    /* class C answering an uplink (RX1 given) keeps the xtime timing; without xtime it is refused */
    static const char cr1[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":2,\"diid\":5,\"pdu\":\"60\",\"RxDelay\":1,"
                              "\"RX1DR\":5,\"RX1Freq\":868100000,\"RX2DR\":0,\"RX2Freq\":869525000,\"xtime\":4785074604081153}";
    CHECK(lwc_lbs_parse_dnmsg(cr1, strlen(cr1), &d) == LWC_LBS_OK && !d.asap);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_OK && a.tmst == 1000001U);
    CHECK(lwc_lbs_dn_txpk_now(&d, EU, 0, &a, &b) == LWC_LBS_ERR_ARG);
    static const char cr0[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":2,\"diid\":5,\"pdu\":\"60\",\"RxDelay\":1,"
                              "\"RX1DR\":5,\"RX1Freq\":868100000}";
    CHECK(lwc_lbs_parse_dnmsg(cr0, strlen(cr0), &d) == LWC_LBS_ERR_UNSUPPORTED);
    /* class A with RX2 only and no xtime: still refused */
    static const char ca0[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":0,\"diid\":6,\"pdu\":\"60\",\"RX2DR\":0,"
                              "\"RX2Freq\":869525000}";
    CHECK(lwc_lbs_parse_dnmsg(ca0, strlen(ca0), &d) == LWC_LBS_ERR_UNSUPPORTED);
    /* an asap downlink at a data rate outside EU868 */
    static const char cc9[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"dC\":2,\"diid\":7,\"pdu\":\"60\",\"RX2DR\":9,"
                              "\"RX2Freq\":869525000}";
    CHECK(lwc_lbs_parse_dnmsg(cc9, strlen(cc9), &d) == LWC_LBS_OK && lwc_lbs_dn_txpk_now(&d, EU, 0, &a, &b) == LWC_LBS_ERR_FIELD);

    /* bad: no pdu, odd hex, RX1 half given, DR out of range, xtime as a fraction, not a dnmsg */
    static const char *const bad[] = {
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"RX1DR\":5,\"RX1Freq\":868100000,\"xtime\":5}",
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"600\",\"RX1DR\":5,\"RX1Freq\":868100000,\"xtime\":5}",
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":5,\"xtime\":5}",
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":16,\"RX1Freq\":1,\"xtime\":5}",
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":5,\"RX1Freq\":1,\"xtime\":5.5}",
        "{\"msgtype\":\"dnsched\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":5,\"RX1Freq\":1,\"xtime\":5}",
        "{\"msgtype\":\"dnmsg\",\"DevEui\":\"0\\\"1\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":5,\"RX1Freq\":1,\"xtime\":5}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const lwc_lbs_err_t e = lwc_lbs_parse_dnmsg(bad[i], strlen(bad[i]), &d);
        if (e != LWC_LBS_ERR_FIELD) {
            fprintf(stderr, "bad[%zu]: %d\n", i, (int)e);
        }
        CHECK(e == LWC_LBS_ERR_FIELD);
    }
    CHECK(lwc_lbs_parse_dnmsg("{\"msgtype\":\"dnmsg\"", 18, &d) == LWC_LBS_ERR_JSON);
    /* a data rate outside EU868 is refused when the window is built */
    static const char dr9[] = "{\"msgtype\":\"dnmsg\",\"DevEui\":\"01\",\"diid\":3,\"pdu\":\"60\",\"RX1DR\":9,"
                              "\"RX1Freq\":868100000,\"xtime\":4785074604081153}";
    CHECK(lwc_lbs_parse_dnmsg(dr9, strlen(dr9), &d) == LWC_LBS_OK);
    CHECK(lwc_lbs_dn_txpk(&d, EU, 0x11, &a, &b) == LWC_LBS_ERR_FIELD);
}

static void router_config(void)
{
    /* shaped like TTS's EU868 answer (pfconfig), one concentrator */
    static const char rc[] =
        "{\"msgtype\":\"router_config\",\"NetID\":null,\"JoinEui\":null,\"region\":\"EU863\",\"hwspec\":\"sx1301/1\","
        "\"freq_range\":[863000000,870000000],\"DRs\":[[12,125,0],[11,125,0],[10,125,0],[9,125,0],[8,125,0],[7,125,0],"
        "[7,250,0],[0,0,0],[-1,0,0],[-1,0,0],[-1,0,0],[-1,0,0],[-1,0,0],[-1,0,0],[-1,0,0],[-1,0,0]],"
        "\"sx1301_conf\":[{\"radio_0\":{\"enable\":true,\"freq\":867500000},\"radio_1\":{\"enable\":true,\"freq\":868500000},"
        "\"chan_FSK\":{\"enable\":true,\"radio\":1,\"if\":300000},"
        "\"chan_Lora_std\":{\"enable\":true,\"radio\":1,\"if\":-200000,\"bandwidth\":250000,\"spread_factor\":7},"
        "\"chan_multiSF_0\":{\"enable\":true,\"radio\":1,\"if\":-400000},\"chan_multiSF_1\":{\"enable\":true,\"radio\":1,\"if\":-200000},"
        "\"chan_multiSF_2\":{\"enable\":true,\"radio\":1,\"if\":0},\"chan_multiSF_3\":{\"enable\":true,\"radio\":0,\"if\":-400000},"
        "\"chan_multiSF_4\":{\"enable\":true,\"radio\":0,\"if\":-200000},\"chan_multiSF_5\":{\"enable\":true,\"radio\":0,\"if\":0},"
        "\"chan_multiSF_6\":{\"enable\":true,\"radio\":0,\"if\":200000},\"chan_multiSF_7\":{\"enable\":true,\"radio\":0,\"if\":400000}}],"
        "\"nocca\":true,\"nodc\":true,\"nodwell\":true,\"MuxTime\":1554300787.123}";
    lwc_lbs_router_config_t c;
    CHECK(lwc_lbs_parse_router_config(rc, strlen(rc), &c) == LWC_LBS_OK);
    CHECK(strcmp(c.region, "EU863") == 0 && c.freq_min == 863000000 && c.freq_max == 870000000 && lwc_lbs_router_drs_ok(&c, EU));
    CHECK(c.radio_hz[0] == 867500000 && c.radio_hz[1] == 868500000 && c.multisf == 0xFF);
    CHECK(c.chan_radio[0] == 1 && c.chan_if[0] == -400000 && c.chan_radio[7] == 0 && c.chan_if[7] == 400000);

    /* a US915 table does not match */
    static const char us[] = "{\"msgtype\":\"router_config\",\"DRs\":[[10,125,0],[9,125,0],[8,125,0],[7,125,0],"
                             "[8,500,0],[-1,0,0],[-1,0,0],[-1,0,0]]}";
    CHECK(lwc_lbs_parse_router_config(us, strlen(us), &c) == LWC_LBS_OK && !lwc_lbs_router_drs_ok(&c, EU) && c.multisf == 0);
    static const char shortdrs[] = "{\"msgtype\":\"router_config\",\"DRs\":[[12,125,0]]}";
    CHECK(lwc_lbs_parse_router_config(shortdrs, strlen(shortdrs), &c) == LWC_LBS_OK && !lwc_lbs_router_drs_ok(&c, EU));
    static const char notrc[] = "{\"msgtype\":\"dnmsg\"}";
    CHECK(lwc_lbs_parse_router_config(notrc, strlen(notrc), &c) == LWC_LBS_ERR_FIELD);
    static const char badif[] = "{\"msgtype\":\"router_config\",\"sx1301_conf\":[{\"chan_multiSF_0\":{\"if\":\"x\"}}]}";
    CHECK(lwc_lbs_parse_router_config(badif, strlen(badif), &c) == LWC_LBS_ERR_FIELD);
}

/* The router_config TTN eu1 sent this gateway (2026-11-23), as received. */
static void router_config_ttn(void)
{
    static const char rc[] =
        "{\"msgtype\":\"router_config\",\"NetID\":null,\"JoinEui\":null,\"region\":\"EU863\",\"hwspec\""
        ":\"sx1301/1\",\"freq_range\":[863000000,870000000],\"DRs\":[[12,125,0],[11,125,0],[10,"
        "125,0],[9,125,0],[8,125,0],[7,125,0],[7,250,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],["
        "0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0]],\"sx1301_conf\":[{\"radio_0\":{\"enable\":true"
        ",\"freq\":867500000},\"radio_1\":{\"enable\":true,\"freq\":868500000},\"chan_multiSF_0\":{"
        "\"enable\":true,\"radio\":1,\"if\":-400000},\"chan_multiSF_1\":{\"enable\":true,\"radio\":1,"
        "\"if\":-200000},\"chan_multiSF_2\":{\"enable\":true,\"radio\":1,\"if\":0},\"chan_multiSF_3\""
        ":{\"enable\":true,\"radio\":0,\"if\":-400000},\"chan_multiSF_4\":{\"enable\":true,\"radio\":"
        "0,\"if\":-200000},\"chan_multiSF_5\":{\"enable\":true,\"radio\":0,\"if\":0},\"chan_multiSF_"
        "6\":{\"enable\":true,\"radio\":0,\"if\":200000},\"chan_multiSF_7\":{\"enable\":true,\"radio\""
        ":0,\"if\":400000},\"chan_Lora_std\":{\"enable\":true,\"radio\":1,\"if\":-200000,\"bandwidth"
        "\":250000,\"spread_factor\":7},\"chan_FSK\":{\"enable\":true,\"radio\":1,\"if\":300000,\"dat"
        "arate\":50000}}],\"nocca\":true,\"nodc\":true,\"nodwell\":true,\"MuxTime\":1795449071.152"
        "2963,\"bcning\":{\"DR\":3,\"layout\":[2,8,17],\"freqs\":[869525000]}}";
    lwc_lbs_router_config_t c;
    CHECK(lwc_lbs_parse_router_config(rc, strlen(rc), &c) == LWC_LBS_OK);
    CHECK(strcmp(c.region, "EU863") == 0 && lwc_lbs_router_drs_ok(&c, EU) && c.have_sx1301 && c.multisf == 0xFF);
    CHECK(c.std_on && c.std_bw_hz == 250000 && c.std_sf == 7 && c.std_radio == 1 && c.std_if == -200000);
    CHECK(c.fsk_on && c.fsk_radio == 1 && c.fsk_if == 300000);
    lwc_plan_t p, d;
    CHECK(lwc_lbs_router_plan(&c, EU, &p));
    /* TTN's plan is the built-in one plus FSK at 868.8 MHz */
    (void)lwc_plan_default(&d, EU);
    CHECK(!lwc_plan_equal(&p, &d));
    d.fsk.enable = true;
    CHECK(lwc_plan_equal(&p, &d) && lwc_plan_hz(&p, &p.fsk) == 868800000);
    /* ADR-0024: the same router_config for a gateway set to another region. AS923
     * DR0-6 equal EU868's, so the DRs pass; the plan does not (default channels). */
    const lwc_region_t *as1 = lwc_region(LWC_REGION_AS923_1);
    CHECK(lwc_lbs_router_drs_ok(&c, as1) && !lwc_lbs_router_plan(&c, as1, &p));
    CHECK(!lwc_lbs_router_drs_ok(&c, lwc_region(LWC_REGION_US915)) && !lwc_lbs_router_drs_ok(&c, NULL));
    /* no sx1301_conf: no plan */
    static const char bare[] = "{\"msgtype\":\"router_config\",\"region\":\"EU863\"}";
    CHECK(lwc_lbs_parse_router_config(bare, strlen(bare), &c) == LWC_LBS_OK && !lwc_lbs_router_plan(&c, EU, &p));
}

int main(void)
{
    drs();
    xtime();
    version();
    uplinks();
    dntxed();
    msgtypes();
    dnmsgs();
    router_config();
    router_config_ttn();
    if (failures != 0) {
        fprintf(stderr, "test_lbs: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_lbs: all passed\n");
    return 0;
}
