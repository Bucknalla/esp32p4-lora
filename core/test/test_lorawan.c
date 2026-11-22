/*
 * LoRaWAN 1.0.x frames. Vectors from ChirpStack (chirpstack/lrwn, MIT):
 * phy_payload.rs doc examples (join-request, 1.0 join-accept, 1.0 confirmed
 * uplink, 1.0 downlink) and keys.rs test lorawan_1_0. Test keys only.
 */
#include "lwc/lorawan.h"

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

static size_t hex(const char *s, uint8_t *out)
{
    size_t i = 0;
    for (; s[2 * i] != '\0'; i++) {
        unsigned v;
        sscanf(&s[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return i;
}

static void join_request(void)
{
    uint8_t key[16], f[32];
    hex("0102030405060708090a0b0c0d0e0f10", key);
    const size_t n = hex("0001010101010101010202020202020202030309b97b32", f);
    lwc_lw_join_req_t jr;
    CHECK(lwc_lw_mtype(f, n) == LWC_LW_JOIN_REQUEST);
    CHECK(lwc_lw_join_req_parse(f, n, &jr) == LWC_LW_OK);
    CHECK(jr.join_eui == 0x0101010101010101ULL && jr.dev_eui == 0x0202020202020202ULL);
    CHECK(jr.dev_nonce == 0x0303);
    CHECK(lwc_lw_join_req_mic_ok(key, f, n));
    f[5] ^= 1U;
    CHECK(!lwc_lw_join_req_mic_ok(key, f, n));
    /* byte order: wire 08..01 is JoinEUI 0102030405060708 (test_non_proprietary) */
    const size_t m = hex("000807060504030201010203040506070800040102030" "4", f);
    CHECK(lwc_lw_join_req_parse(f, m, &jr) == LWC_LW_OK);
    CHECK(jr.join_eui == 0x0102030405060708ULL && jr.dev_eui == 0x0807060504030201ULL);
    CHECK(jr.dev_nonce == 1024);
    CHECK(lwc_lw_join_req_parse(f, m - 1U, &jr) == LWC_LW_ERR_SHORT);
    f[0] = 0x01; /* Major 1 */
    CHECK(lwc_lw_join_req_parse(f, m, &jr) == LWC_LW_ERR_MAJOR);
}

static void join_accept(void)
{
    uint8_t key[16], want[17], out[40];
    hex("0102030405060708090a0b0c0d0e0f10", key);
    hex("2023cf335489aae3183c0be0baa8dee5f3", want);
    const size_t n = lwc_lw_join_accept(key, 0x010101, 0x020202, 0x01020304, 0, 0, NULL, out, sizeof out);
    CHECK(n == 17U && memcmp(out, want, 17) == 0);
    CHECK(lwc_lw_join_accept(key, 1, 2, 3, 0, 0, NULL, out, 16) == 0U);
    uint8_t cf[16];
    const uint32_t fr[5] = {867100000, 867300000, 867500000, 867700000, 867900000};
    lwc_lw_cflist_freqs(fr, cf);
    CHECK(cf[0] == 0x18 && cf[1] == 0x4F && cf[2] == 0x84 && cf[15] == 0); /* 8671000 = 0x844F18 */
    CHECK(lwc_lw_join_accept(key, 1, 2, 3, 0, 1, cf, out, sizeof out) == 33U);
}

static void session_keys(void)
{
    uint8_t key[16], nwk[16], app[16];
    hex("01020304050607080102030405060708", key);
    static const uint8_t want_nwk[16] = {223, 83, 195, 95, 48, 52, 204, 206, 208, 255, 53, 76, 112, 222, 4, 223};
    static const uint8_t want_app[16] = {146, 123, 156, 145, 17, 131, 207, 254, 76, 178, 255, 75, 117, 84, 95, 109};
    lwc_lw_session_keys(key, 65536, 0x010203, 258, nwk, app);
    CHECK(memcmp(nwk, want_nwk, 16) == 0);
    CHECK(memcmp(app, want_app, 16) == 0);
}

static void uplink(void)
{
    uint8_t nwk[16], app[16], f[64];
    hex("0102030405060708090a0b0c0d0e0f10", nwk);
    hex("100f0e0d0c0b0a090807060504030201", app);
    const size_t n = hex("80040302010300000673070ae264d4f7e117d2c0", f);
    lwc_lw_data_t d;
    CHECK(lwc_lw_data_parse(f, n, &d) == LWC_LW_OK);
    CHECK(d.mtype == LWC_LW_CONFIRMED_UP && d.dev_addr == 0x01020304U && d.fcnt16 == 0);
    CHECK(d.fopts_len == 3 && d.fopts == &f[8] && d.fopts[0] == 0x06);
    CHECK(d.has_port && d.fport == 10 && d.frm_len == 4);
    CHECK(lwc_lw_data_mic_ok(nwk, false, d.dev_addr, 0, f, n));
    CHECK(!lwc_lw_data_mic_ok(nwk, false, d.dev_addr, 0x10000, f, n)); /* other FCnt */
    CHECK(!lwc_lw_data_mic_ok(nwk, true, d.dev_addr, 0, f, n));        /* other direction */
    uint8_t plain[4];
    lwc_lw_crypt(app, false, d.dev_addr, 0, d.frm, d.frm_len, plain);
    CHECK(plain[0] == 1 && plain[1] == 2 && plain[2] == 3 && plain[3] == 4);
    /* malformed */
    CHECK(lwc_lw_data_parse(f, 11, &d) == LWC_LW_ERR_SHORT);
    uint8_t g[12] = {0x40, 1, 2, 3, 4, 0x0F, 0, 0, 0, 0, 0, 0}; /* FOptsLen past the end */
    CHECK(lwc_lw_data_parse(g, sizeof g, &d) == LWC_LW_ERR_SHORT);
    uint8_t h[16] = {0x40, 1, 2, 3, 4, 0x01, 0, 0, 0x02, 0x00, 0xAA, 0, 0, 0, 0, 0}; /* FOpts + port 0 */
    CHECK(lwc_lw_data_parse(h, 15, &d) == LWC_LW_ERR_FORMAT);
    uint8_t j[12] = {0x00, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0}; /* join-request MType */
    CHECK(lwc_lw_data_parse(j, sizeof j, &d) == LWC_LW_ERR_MTYPE);
    uint8_t e[12] = {0x40, 1, 2, 3, 4, 0, 5, 0, 0, 0, 0, 0}; /* no port, no payload */
    CHECK(lwc_lw_data_parse(e, sizeof e, &d) == LWC_LW_OK && !d.has_port && d.fcnt16 == 5);
}

static void downlink(void)
{
    uint8_t key[16], f[64];
    hex("08070605040302010807060504030201", key);
    const size_t n = hex("6004030201000a00e2c9604164c97d76f9ea8e1a792ba0879b85243e5af5", f);
    lwc_lw_data_t d;
    CHECK(lwc_lw_data_parse(f, n, &d) == LWC_LW_OK);
    CHECK(d.mtype == LWC_LW_UNCONFIRMED_DOWN && d.dev_addr == 0x01020304U && d.fcnt16 == 10);
    CHECK(lwc_lw_data_mic_ok(key, true, d.dev_addr, 10, f, n));
    uint8_t mic[4];
    lwc_lw_data_mic(key, true, d.dev_addr, 10, f, n - 4U, mic);
    CHECK(memcmp(mic, &f[n - 4U], 4) == 0);

    /* rebuild it byte for byte: decrypt FRMPayload with any key, build with the same key */
    uint8_t app[16], plain[32], g[64];
    hex("00112233445566778899aabbccddeeff", app);
    CHECK(d.has_port && d.frm_len <= sizeof plain);
    lwc_lw_crypt(app, true, d.dev_addr, 10, d.frm, d.frm_len, plain);
    size_t m = lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 0x01020304U, d.fctrl, 10, NULL, 0, true, d.fport,
                                 plain, d.frm_len, key, app, g, sizeof g);
    CHECK(m == n && memcmp(g, f, n) == 0);
    CHECK(lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 0x01020304U, 0, 10, NULL, 0, true, d.fport, plain,
                            d.frm_len, key, app, g, n - 1U) == 0U); /* no room */

    /* ACK only: 12 bytes, no FPort; the counter's high bits are in the MIC only */
    m = lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 0x00000042U, LWC_LW_FCTRL_ACK | LWC_LW_FCTRL_ADR, 0x10005,
                          NULL, 0, false, 0, NULL, 0, key, NULL, g, sizeof g);
    CHECK(m == 12U && g[0] == 0x60 && g[5] == 0xA0 && g[6] == 5 && g[7] == 0);
    CHECK(lwc_lw_data_parse(g, m, &d) == LWC_LW_OK && !d.has_port && d.fopts_len == 0);
    CHECK(lwc_lw_data_mic_ok(key, true, 0x42, 0x10005, g, m) && !lwc_lw_data_mic_ok(key, true, 0x42, 5, g, m));

    /* MAC commands in FOpts (FOptsLen set from the length, FCtrl's low bits ignored) */
    static const uint8_t fo[3] = {0x02, 0x0A, 0x01};
    m = lwc_lw_data_build(LWC_LW_CONFIRMED_DOWN, 7, 0x0F, 1, fo, 3, false, 0, NULL, 0, key, NULL, g, sizeof g);
    CHECK(m == 15U && g[0] == 0xA0 && g[5] == 0x03 && memcmp(&g[8], fo, 3) == 0);
    /* MAC commands on port 0: encrypted with the network key */
    m = lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 7, 0, 2, NULL, 0, true, 0, fo, 3, key, NULL, g, sizeof g);
    CHECK(m == 16U && g[8] == 0 && lwc_lw_data_parse(g, m, &d) == LWC_LW_OK);
    lwc_lw_crypt(key, true, 7, 2, d.frm, d.frm_len, plain);
    CHECK(memcmp(plain, fo, 3) == 0);
    /* refused */
    CHECK(lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 7, 0, 2, fo, 3, true, 0, fo, 3, key, NULL, g, sizeof g) == 0U);
    CHECK(lwc_lw_data_build(LWC_LW_JOIN_ACCEPT, 7, 0, 2, NULL, 0, false, 0, NULL, 0, key, NULL, g, sizeof g) == 0U);
    CHECK(lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 7, 0, 2, fo, 16, false, 0, NULL, 0, key, NULL, g, sizeof g) == 0U);
    CHECK(lwc_lw_data_build(LWC_LW_UNCONFIRMED_DOWN, 7, 0, 2, NULL, 0, true, 1, fo, 3, key, NULL, g, sizeof g) == 0U);
}

static void fcnt(void)
{
    CHECK(lwc_lw_fcnt32(0, 0) == 0);
    CHECK(lwc_lw_fcnt32(5, 7) == 7);
    CHECK(lwc_lw_fcnt32(0xFFFF, 0x0001) == 0x10001U); /* 16-bit wrap */
    CHECK(lwc_lw_fcnt32(0x12345, 0x2345) == 0x12345U); /* same: caller rejects a replay */
    CHECK(lwc_lw_fcnt32(0x12345, 0x2344) == 0x22344U); /* older 16 bits: next epoch */
}

int main(void)
{
    join_request();
    join_accept();
    session_keys();
    uplink();
    downlink();
    fcnt();
    if (failures != 0) {
        fprintf(stderr, "test_lorawan: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_lorawan: all passed\n");
    return 0;
}
