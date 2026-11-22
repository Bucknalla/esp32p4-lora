/* Local network server: joins, uplinks, routing, downlink timing. Test keys only. */
#include "lwc/ns.h"

#include <stdio.h>
#include <string.h>

#include "lwc/aes.h"
#include "lwc/lorawan.h"

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static const uint8_t KEY[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
#define DEV_EUI 0x0011223344556677ULL
#define JOIN_EUI 0x70B3D57ED0000000ULL

static void put_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

/* What a device sends. */
static size_t join_request(uint8_t f[23], const uint8_t key[16], uint64_t dev_eui, uint16_t nonce)
{
    f[0] = 0x00;
    put_le(&f[1], JOIN_EUI, 8);
    put_le(&f[9], dev_eui, 8);
    put_le(&f[17], nonce, 2);
    uint8_t mac[16];
    lwc_aes_cmac(key, f, 19, mac);
    memcpy(&f[19], mac, 4);
    return 23;
}

static size_t uplink(uint8_t *f, const uint8_t nwk[16], const uint8_t app[16], uint32_t addr,
                     uint32_t fcnt, uint8_t port, const char *text, bool confirmed)
{
    const size_t len = strlen(text);
    f[0] = confirmed ? 0x80 : 0x40;
    put_le(&f[1], addr, 4);
    f[5] = 0x80; /* ADR */
    put_le(&f[6], fcnt, 2);
    f[8] = port;
    lwc_lw_crypt(app, false, addr, fcnt, (const uint8_t *)text, len, &f[9]);
    lwc_lw_data_mic(nwk, false, addr, fcnt, f, 9U + len, &f[9 + len]);
    return 13U + len;
}

static lwc_ns_t ns; /* large: keep off the stack */

int main(void)
{
    lwc_ns_init(&ns);
    size_t idx;
    CHECK(lwc_ns_add(&ns, DEV_EUI, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, "sensor-1\n", &idx) == LWC_NS_OK);
    CHECK(strcmp(ns.dev[idx].name, "sensor-1?") == 0); /* control bytes masked */
    CHECK(lwc_ns_add(&ns, DEV_EUI, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, NULL, NULL) == LWC_NS_ERR_EXISTS);
    CHECK(lwc_ns_add(&ns, 1, 2, KEY, (lwc_ns_mac_t)7, NULL, NULL) == LWC_NS_ERR_ARG);

    /* join */
    uint8_t jr[23], acc[40];
    size_t alen = 0, jidx = 99;
    join_request(jr, KEY, DEV_EUI, 5);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, &jidx) == LWC_NS_JOIN_ACCEPT);
    CHECK(alen == 33U && jidx == idx && acc[0] == 0x20);
    /* the device decrypts with AES encrypt and checks the MIC */
    uint8_t plain[33];
    plain[0] = acc[0];
    lwc_aes128_t ctx;
    lwc_aes128_init(&ctx, KEY);
    lwc_aes128_encrypt(&ctx, &acc[1], &plain[1]);
    lwc_aes128_encrypt(&ctx, &acc[17], &plain[17]);
    uint8_t mac[16];
    lwc_aes_cmac(KEY, plain, 29, mac);
    CHECK(memcmp(mac, &plain[29], 4) == 0);
    const uint32_t jn = (uint32_t)(plain[1] | (plain[2] << 8) | (plain[3] << 16));
    const uint32_t addr = (uint32_t)plain[7] | ((uint32_t)plain[8] << 8) | ((uint32_t)plain[9] << 16) |
                          ((uint32_t)plain[10] << 24);
    CHECK(plain[4] == 0 && plain[5] == 0 && plain[6] == 0); /* NetID 000000 */
    CHECK(addr >= 1U && addr <= LWC_NS_ADDR_MAX && addr == ns.dev[idx].dev_addr);
    CHECK(plain[11] == LWC_NS_DL_SETTINGS && plain[12] == LWC_NS_RX_DELAY);
    CHECK(plain[13] == 0x18 && plain[14] == 0x4F && plain[15] == 0x84 && plain[28] == 0); /* 867.1 MHz, type 0 */

    /* replays and strangers */
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_REPLAY);
    join_request(jr, KEY, DEV_EUI, 4); /* 1.0.4: must increase */
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_REPLAY);
    join_request(jr, KEY, 0x99, 6);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_NOT_OURS);
    uint8_t bad[16];
    memcpy(bad, KEY, 16);
    bad[0] ^= 1U;
    join_request(jr, bad, DEV_EUI, 6);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_BAD_MIC);
    CHECK(lwc_ns_join(&ns, jr, 22, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_MALFORMED);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, 32, &alen, NULL) == LWC_NS_JOIN_MALFORMED);

    /* uplinks with the session keys the device derives */
    uint8_t nwk[16], app[16], f[64];
    lwc_lw_session_keys(KEY, jn, 0, 5, nwk, app);
    lwc_ns_uplink_t up;
    size_t n = uplink(f, nwk, app, addr, 0, 1, "hello", false);
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_OK);
    CHECK(up.idx == idx && up.fcnt == 0 && up.port == 1 && up.len == 5 && memcmp(up.payload, "hello", 5) == 0);
    CHECK(up.adr && !up.confirmed);
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_REPLAY);
    n = uplink(f, nwk, app, addr, 0xFFFF, 2, "x", true);
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_OK && up.fcnt == 0xFFFFU && up.confirmed);
    n = uplink(f, nwk, app, addr, 0x10001, 2, "y", false); /* 16-bit wrap */
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_OK && up.fcnt == 0x10001U);
    n = uplink(f, nwk, app, addr + 1U, 0x10002, 2, "z", false);
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_NOT_OURS);
    n = uplink(f, nwk, app, addr, 0x10002, 2, "z", false);
    f[n - 1U] ^= 1U;
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_BAD_MIC);
    f[0] = 0x60; /* a downlink is not an uplink */
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_MALFORMED);

    /* rejoin keeps the address, resets counters */
    join_request(jr, KEY, DEV_EUI, 6);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    CHECK(ns.dev[idx].dev_addr == addr && !ns.dev[idx].has_up);

    /* 1.0.2: random nonces, recent ones rejected */
    size_t i2;
    CHECK(lwc_ns_add(&ns, 0x42, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, NULL, &i2) == LWC_NS_OK);
    join_request(jr, KEY, 0x42, 900);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    join_request(jr, KEY, 0x42, 3);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    join_request(jr, KEY, 0x42, 900);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_REPLAY);
    CHECK(ns.dev[i2].dev_addr != addr);

    /* remove wipes the keys */
    CHECK(lwc_ns_remove(&ns, 0x42) == LWC_NS_OK && !ns.dev[i2].used && ns.dev[i2].app_key[0] == 0);
    CHECK(lwc_ns_remove(&ns, 0x42) == LWC_NS_ERR_NOT_FOUND);

    /* table full */
    lwc_ns_t full;
    lwc_ns_init(&full);
    for (uint64_t e = 1; e <= LWC_NS_MAX_DEVICES; e++) {
        CHECK(lwc_ns_add(&full, e, 0, KEY, LWC_NS_MAC_1_0_4, NULL, NULL) == LWC_NS_OK);
    }
    CHECK(lwc_ns_add(&full, 999, 0, KEY, LWC_NS_MAC_1_0_4, NULL, NULL) == LWC_NS_ERR_FULL);

    /* routing */
    CHECK(lwc_ns_forward(LWC_MODE_TTN, true) && lwc_ns_forward(LWC_MODE_TTN, false));
    CHECK(!lwc_ns_forward(LWC_MODE_STANDALONE, true) && !lwc_ns_forward(LWC_MODE_STANDALONE, false));
    CHECK(!lwc_ns_forward(LWC_MODE_HYBRID, true) && lwc_ns_forward(LWC_MODE_HYBRID, false));

    /* downlink timing */
    lwc_txpk_t tx;
    lwc_ns_downlink(false, 1000, 868100000, 9, 125, LWC_NS_JOIN_DELAY_US, 13, acc, 33, &tx);
    CHECK(tx.tmst == 5001000U && tx.freq_hz == 868100000U && tx.sf == 9 && tx.ipol && tx.ncrc && tx.size == 33);
    lwc_ns_downlink(true, 0xFFFFFFFFU, 868100000, 9, 125, LWC_NS_RX1_DELAY_US, 13, NULL, 0, &tx);
    CHECK(tx.tmst == 1999999U && tx.freq_hz == LWC_NS_RX2_FREQ_HZ && tx.sf == 12 && tx.bw_khz == 125);

    if (failures != 0) {
        fprintf(stderr, "test_ns: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ns: all passed\n");
    return 0;
}
