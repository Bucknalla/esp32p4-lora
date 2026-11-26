/* Local network server: joins, uplinks, routing, downlink timing. Test keys only. */
#include "lwc/ns.h"

#include <stdio.h>
#include <string.h>

#include "lwc/aes.h"
#include "lwc/lorawan.h"
#include "lwc/mac.h"

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

/* An uplink with MAC commands in FOpts (or none) and FCtrl bits. */
static size_t uplink_mac(uint8_t *f, const uint8_t nwk[16], uint32_t addr, uint32_t fcnt, const uint8_t *fopts,
                         size_t fo, uint8_t fctrl, bool confirmed)
{
    return lwc_lw_data_build(confirmed ? LWC_LW_CONFIRMED_UP : LWC_LW_UNCONFIRMED_UP, addr, fctrl, fcnt, fopts, fo,
                             false, 0, NULL, 0, nwk, NULL, f, 64);
}

static lwc_ns_t ns; /* large: keep off the stack */

/* Class A answers, checked the way the device reads them. */
static void answers(size_t idx, uint32_t addr, const uint8_t nwk[16], const uint8_t app[16], uint32_t fcnt)
{
    uint8_t f[64];
    lwc_ns_uplink_t up;
    lwc_ns_dl_t dl;
    lwc_ns_rx_t rx = {.snr10 = 0, .sf = 9, .bw_khz = 125, .now_s = 1000}; /* no ADR change at 0 dB, SF9 */
    lwc_lw_data_t d;

    /* unconfirmed, no MAC commands: nothing to send */
    size_t n = uplink_mac(f, nwk, addr, fcnt, NULL, 0, 0, false);
    lwc_ns_up_res_t r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK);
    CHECK(!lwc_ns_answer(&ns, r, &up, &rx, &dl));

    /* confirmed: ACK, plus DevStatusReq (first downlink of the session) */
    const uint32_t down0 = ns.dev[idx].fcnt_down;
    n = uplink(f, nwk, app, addr, ++fcnt, 3, "c", true);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer(&ns, r, &up, &rx, &dl));
    CHECK(dl.ack && dl.dev_status && !dl.link_check && dl.fcnt == down0 && ns.dev[idx].fcnt_down == down0 + 1U);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.mtype == LWC_LW_UNCONFIRMED_DOWN);
    CHECK(d.dev_addr == addr && (d.fctrl & LWC_LW_FCTRL_ACK) != 0U && !d.has_port);
    CHECK(d.fopts_len == 1 && d.fopts[0] == LWC_MAC_DEV_STATUS);
    CHECK(lwc_lw_data_mic_ok(nwk, true, addr, down0, dl.frame, dl.len));

    /* the device missed it and repeats: ACK again with a new counter, no payload, no new status request */
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_RETRANS && up.confirmed && up.len == 0);
    CHECK(lwc_ns_answer(&ns, r, &up, &rx, &dl) && dl.ack && !dl.dev_status && dl.fcnt == down0 + 1U);
    for (unsigned k = 1; k < LWC_NS_RETRANS_MAX; k++) {
        CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_RETRANS);
    }
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_REPLAY); /* then a replay */

    /* LinkCheckReq + DevStatusAns + DeviceTimeReq (clock not set: no time answer) */
    static const uint8_t mreq[] = {0x02, 0x06, 0xFE, 0x05, 0x0D};
    n = uplink_mac(f, nwk, addr, ++fcnt, mreq, sizeof mreq, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer(&ns, r, &up, &rx, &dl));
    CHECK(!dl.ack && dl.link_check && !dl.device_time && !dl.dev_status);
    CHECK(ns.link[idx].has_status && ns.link[idx].battery == 0xFE && ns.link[idx].margin == 5);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts_len == 3);
    CHECK(d.fopts[0] == 0x02 && d.fopts[1] == 12 && d.fopts[2] == 1); /* 0 dB - (-12.5 dB) = 12 dB, 1 gateway */

    /* DeviceTimeReq with the clock set, on port 0 */
    rx.time_ok = true;
    rx.gps_s = 0x01020304U;
    rx.gps_frac = 0x40;
    static const uint8_t treq[] = {0x0D};
    n = lwc_lw_data_build(LWC_LW_UNCONFIRMED_UP, addr, 0, ++fcnt, NULL, 0, true, 0, treq, 1, nwk, NULL, f, 64);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer(&ns, r, &up, &rx, &dl) && dl.device_time && !dl.link_check);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts_len == 6);
    CHECK(d.fopts[0] == 0x0D && d.fopts[1] == 0x04 && d.fopts[4] == 0x01 && d.fopts[5] == 0x40);

    /* ADRACKReq: an empty downlink; a day later DevStatusReq rides along again */
    rx.now_s += LWC_NS_STATUS_PERIOD_S;
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, LWC_LW_FCTRL_ADR | LWC_LW_FCTRL_ADR_ACK_REQ, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && up.adr_ack_req && lwc_ns_answer(&ns, r, &up, &rx, &dl));
    CHECK(!dl.ack && dl.dev_status);

    /* not for us */
    CHECK(!lwc_ns_answer(&ns, LWC_NS_UP_BAD_MIC, &up, &rx, &dl));
    CHECK(!lwc_ns_answer(&ns, LWC_NS_UP_REPLAY, &up, &rx, &dl));
}

/* Application downlinks (ADR-0018), decoded the way the device reads them. */
static bool app_ok(const lwc_ns_dl_t *dl, const uint8_t nwk[16], const uint8_t app[16], uint32_t addr, uint8_t port,
                   const uint8_t *data, size_t len, bool fpending)
{
    lwc_lw_data_t d;
    uint8_t plain[LWC_NS_DL_MAX];
    if (lwc_lw_data_parse(dl->frame, dl->len, &d) != LWC_LW_OK ||
        d.mtype != (dl->confirmed ? LWC_LW_CONFIRMED_DOWN : LWC_LW_UNCONFIRMED_DOWN) ||
        d.dev_addr != addr || !d.has_port || d.fport != port || d.frm_len != len ||
        ((d.fctrl & LWC_LW_FCTRL_FPENDING) != 0U) != fpending ||
        !lwc_lw_data_mic_ok(nwk, true, addr, dl->fcnt, dl->frame, dl->len)) {
        return false;
    }
    lwc_lw_crypt(app, true, addr, dl->fcnt, d.frm, d.frm_len, plain);
    return len == 0U || memcmp(plain, data, len) == 0;
}

static void app_downlinks(size_t idx, uint32_t addr, const uint8_t nwk[16], const uint8_t app[16], uint32_t fcnt)
{
    uint8_t f[64], big[230];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i * 7U + 1U);
    }
    lwc_ns_uplink_t up;
    lwc_ns_dl_t dl;
    lwc_ns_rx_t rx = {.snr10 = 0, .sf = 9, .bw_khz = 125, .now_s = 2000}; /* RX1 DR3: 115 B */
    ns.dev[idx].rx2_dr = 3;                                                 /* RX2 DR3: 115 B */
    ns.link[idx].status_asked = true;
    ns.link[idx].status_req_s = rx.now_s;

    CHECK(lwc_ns_dr(EU, 12, 125) == 0 && lwc_ns_dr(EU, 7, 125) == 5 && lwc_ns_dr(EU, 7, 250) == 6 && lwc_ns_dr(EU, 6, 125) == -1);
    CHECK(lwc_ns_max_frm(EU, 0) == 51U && lwc_ns_max_frm(EU, 2) == 51U && lwc_ns_max_frm(EU, 3) == 115U &&
          lwc_ns_max_frm(EU, 5) == 222U && lwc_ns_max_frm(EU, -1) == 0U && lwc_ns_max_frm(EU, 8) == 0U);

    /* unconfirmed uplink, the largest payload, more queued: FPending */
    size_t n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    lwc_ns_up_res_t r = lwc_ns_uplink(&ns, f, n, &up);
    lwc_ns_app_t a = {.port = 10, .data = big, .len = 115, .more = true};
    uint32_t down = ns.dev[idx].fcnt_down;
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app && !dl.app_too_big);
    CHECK(!dl.ack && !dl.dev_status && dl.fcnt == down && ns.dev[idx].fcnt_down == down + 1U);
    CHECK(app_ok(&dl, nwk, app, addr, 10, big, 115, true));

    /* one byte more: not sent; nothing else to say, so no downlink and no counter used */
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 10, .data = big, .len = 116};
    down = ns.dev[idx].fcnt_down;
    CHECK(r == LWC_NS_UP_OK && !lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app_too_big && !dl.app);
    CHECK(ns.dev[idx].fcnt_down == down);

    /* confirmed uplink: ACK and payload together; the repeat gets them again with a new counter */
    n = uplink(f, nwk, app, addr, ++fcnt, 3, "c", true);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 1, .data = big, .len = 4};
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.ack && dl.app);
    CHECK(app_ok(&dl, nwk, app, addr, 1, big, 4, false));
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_RETRANS && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.ack && dl.app);
    CHECK(dl.fcnt == ns.dev[idx].fcnt_down - 1U && app_ok(&dl, nwk, app, addr, 1, big, 4, false));

    /* MAC answers share the room: LinkCheckAns (3 B) + 112 B fits at 115, 113 B does not */
    static const uint8_t lcr[] = {0x02};
    n = uplink_mac(f, nwk, addr, ++fcnt, lcr, 1, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 2, .data = big, .len = 112};
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.link_check && dl.app);
    CHECK(app_ok(&dl, nwk, app, addr, 2, big, 112, false));
    n = uplink_mac(f, nwk, addr, ++fcnt, lcr, 1, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a.len = 113;
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.link_check && !dl.app &&
          dl.app_too_big);

    /* RX2 at DR0 limits both windows to 51 B, even at SF7 */
    ns.dev[idx].rx2_dr = 0;
    rx.sf = 7;
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 3, .data = big, .len = 52};
    CHECK(r == LWC_NS_UP_OK && !lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app_too_big);
    a.len = 51;
    CHECK(lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && app_ok(&dl, nwk, app, addr, 3, big, 51, false));

    /* the daily DevStatusReq rides along only when there is room for it */
    rx.now_s += LWC_NS_STATUS_PERIOD_S;
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app && !dl.dev_status);
    a.len = 50;
    CHECK(lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app && dl.dev_status);
    CHECK(app_ok(&dl, nwk, app, addr, 3, big, 50, false));

    /* an empty payload on a port; ports 0 and 224 are never sent */
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 5, .data = NULL, .len = 0};
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl));
    CHECK(app_ok(&dl, nwk, app, addr, 5, NULL, 0, false));
    a = (lwc_ns_app_t){.port = 0, .data = big, .len = 1};
    CHECK(!lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app_too_big);
    a.port = 224;
    CHECK(!lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app_too_big);
    CHECK(!lwc_ns_answer_app(&ns, LWC_NS_UP_REPLAY, &up, &rx, &a, &dl));

    /* confirmed downlink: MType 5, read by the device as such; its ACK comes back in FCtrl */
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && !up.ack);
    a = (lwc_ns_app_t){.port = 7, .data = big, .len = 3, .confirmed = true};
    CHECK(lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.app && dl.confirmed && !dl.ack);
    CHECK(lwc_lw_mtype(dl.frame, dl.len) == LWC_LW_CONFIRMED_DOWN && app_ok(&dl, nwk, app, addr, 7, big, 3, false));
    n = uplink_mac(f, nwk, addr, ++fcnt, NULL, 0, LWC_LW_FCTRL_ACK, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    CHECK(r == LWC_NS_UP_OK && up.ack);
    r = lwc_ns_uplink(&ns, f, n, &up); /* the repeat carries the same bit; the queue ignores it there */
    CHECK(r == LWC_NS_UP_RETRANS && up.ack);
    /* no room for the payload: nothing confirmed goes out */
    n = uplink_mac(f, nwk, addr, ++fcnt, lcr, 1, 0, false);
    r = lwc_ns_uplink(&ns, f, n, &up);
    a = (lwc_ns_app_t){.port = 7, .data = big, .len = 51, .confirmed = true};
    CHECK(r == LWC_NS_UP_OK && lwc_ns_answer_app(&ns, r, &up, &rx, &a, &dl) && dl.link_check && !dl.app &&
          !dl.confirmed && lwc_lw_mtype(dl.frame, dl.len) == LWC_LW_UNCONFIRMED_DOWN);
}

/* Class C (ADR-0020): downlinks that answer no uplink */
static void class_c(size_t idx, uint32_t addr, const uint8_t nwk[16], const uint8_t app[16])
{
    uint8_t big[60];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i ^ 0x5AU);
    }
    lwc_ns_dl_t dl;
    lwc_ns_app_t a = {.port = 12, .data = big, .len = 4};
    const uint64_t eui = ns.dev[idx].dev_eui;
    uint32_t down = ns.dev[idx].fcnt_down;
    CHECK(ns.dev[idx].dev_class == LWC_NS_CLASS_A && !lwc_ns_class_c_down(&ns, idx, &a, &dl)); /* class A */
    CHECK(lwc_ns_set_class(&ns, eui, (lwc_ns_class_t)1) == LWC_NS_ERR_ARG);
    CHECK(lwc_ns_set_class(&ns, 0x99, LWC_NS_CLASS_C) == LWC_NS_ERR_NOT_FOUND);
    CHECK(lwc_ns_set_class(&ns, eui, LWC_NS_CLASS_C) == LWC_NS_OK && ns.dev[idx].dev_class == LWC_NS_CLASS_C);
    /* unconfirmed: no FOpts, no ACK; read as the device reads it */
    CHECK(lwc_ns_class_c_down(&ns, idx, &a, &dl) && dl.app && !dl.confirmed && !dl.ack);
    CHECK(dl.fcnt == down && ns.dev[idx].fcnt_down == down + 1U && app_ok(&dl, nwk, app, addr, 12, big, 4, false));
    lwc_lw_data_t d;
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts_len == 0U &&
          (d.fctrl & LWC_LW_FCTRL_ACK) == 0U);
    /* confirmed and more waiting: MType 5 with FPending */
    a = (lwc_ns_app_t){.port = 13, .data = big, .len = 51, .confirmed = true, .more = true};
    CHECK(lwc_ns_class_c_down(&ns, idx, &a, &dl) && dl.confirmed &&
          lwc_lw_mtype(dl.frame, dl.len) == LWC_LW_CONFIRMED_DOWN && app_ok(&dl, nwk, app, addr, 13, big, 51, true));
    /* RX2 at DR0 here: 52 B does not fit; no counter used */
    down = ns.dev[idx].fcnt_down;
    a.len = 52;
    CHECK(!lwc_ns_class_c_down(&ns, idx, &a, &dl) && dl.app_too_big && ns.dev[idx].fcnt_down == down);
    a = (lwc_ns_app_t){.port = 0, .data = big, .len = 1};
    CHECK(!lwc_ns_class_c_down(&ns, idx, &a, &dl));
    /* no proven session (no uplink since the join): nothing */
    ns.dev[idx].has_up = false;
    a.port = 1;
    CHECK(!lwc_ns_class_c_down(&ns, idx, &a, &dl) && ns.dev[idx].fcnt_down == down);
    ns.dev[idx].has_up = true;
    CHECK(lwc_ns_set_class(&ns, eui, LWC_NS_CLASS_A) == LWC_NS_OK);
    /* the ACK wait: default 60 s, 5..3600 s, 0 back to the default */
    CHECK(lwc_ns_c_ack_s(&ns.dev[idx]) == 60U);
    CHECK(lwc_ns_set_c_ack(&ns, eui, 4) == LWC_NS_ERR_ARG && lwc_ns_set_c_ack(&ns, eui, 3601) == LWC_NS_ERR_ARG);
    CHECK(lwc_ns_set_c_ack(&ns, 0x99, 10) == LWC_NS_ERR_NOT_FOUND);
    CHECK(lwc_ns_set_c_ack(&ns, eui, 5) == LWC_NS_OK && lwc_ns_c_ack_s(&ns.dev[idx]) == 5U);
    CHECK(lwc_ns_set_c_ack(&ns, eui, 3600) == LWC_NS_OK && lwc_ns_c_ack_s(&ns.dev[idx]) == 3600U);
    CHECK(lwc_ns_set_c_ack(&ns, eui, 0) == LWC_NS_OK && lwc_ns_c_ack_s(&ns.dev[idx]) == 60U);
    /* timed at the given counter, RX2 form */
    lwc_txpk_t pk;
    lwc_ns_downlink_at(EU, 0x00000100U, 869525000U, 3, 14, big, 5, &pk);
    CHECK(pk.mode == LWC_TXPK_TIMED && pk.tmst == 0x00000100U && pk.freq_hz == 869525000U && pk.sf == 9U &&
          pk.bw_khz == 125U && pk.ipol && pk.ncrc && pk.size == 5U && pk.powe == 14);
}

static void adr_step(void)
{
    uint8_t dr, pw;
    lwc_ns_adr(EU, -200, 0, 3, 5, 5, &dr, &pw); /* at the SF12 floor: 10 dB short, 4 steps up in power */
    CHECK(dr == 0 && pw == 0);
    lwc_ns_adr(EU, 50, 0, 0, 5, 5, &dr, &pw); /* 5 dB at SF12: 15 dB spare, 5 steps: SF7 */
    CHECK(dr == 5 && pw == 0);
    lwc_ns_adr(EU, 100, 0, 0, 5, 5, &dr, &pw); /* 6 steps: SF7, then one power step */
    CHECK(dr == 5 && pw == 1);
    lwc_ns_adr(EU, 100, 5, 0, 5, 5, &dr, &pw); /* 10 dB at SF7: 7.5 dB spare, 2 steps */
    CHECK(dr == 5 && pw == 2);
    lwc_ns_adr(EU, 300, 5, 0, 5, 5, &dr, &pw); /* power stops at the cap */
    CHECK(dr == 5 && pw == 5);
    lwc_ns_adr(EU, 300, 2, 0, 3, 5, &dr, &pw); /* data rate stops at its cap */
    CHECK(dr == 3 && pw == 5);
    lwc_ns_adr(EU, -100, 5, 5, 5, 5, &dr, &pw); /* 12.5 dB short: -4 steps (truncated), power only */
    CHECK(dr == 5 && pw == 1);
    lwc_ns_adr(EU, -4, 5, 2, 5, 5, &dr, &pw); /* 2.9 dB short: no step */
    CHECK(dr == 5 && pw == 2);
    lwc_ns_adr(EU, -6, 5, 2, 5, 5, &dr, &pw); /* 3.1 dB short: one step */
    CHECK(dr == 5 && pw == 1);
    lwc_ns_adr(EU, 54, 5, 2, 5, 5, &dr, &pw); /* 2.9 dB spare: no step */
    CHECK(dr == 5 && pw == 2);
}

/* ADR end to end: history, LinkADRReq, its answer, a refusal. */
static void adr_flow(void)
{
    static const uint64_t eui = 0x77;
    size_t idx = 0, alen = 0;
    uint8_t jr[23], acc[40], nwk[16], app[16], f[64];
    CHECK(lwc_ns_add(&ns, eui, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, "adr", &idx) == LWC_NS_OK);
    join_request(jr, KEY, eui, 1);
    const uint32_t jn = ns.join_nonce;
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    lwc_lw_session_keys(KEY, jn, 0, 1, nwk, app);
    const uint32_t addr = ns.dev[idx].dev_addr;
    lwc_ns_uplink_t up;
    lwc_ns_dl_t dl;
    lwc_lw_data_t d;
    lwc_ns_rx_t rx = {.snr10 = 50, .sf = 12, .bw_khz = 125, .now_s = 1};
    uint32_t fc = 0;

    /* no ADR bit: no history, no request */
    for (unsigned k = 0; k < LWC_NS_ADR_HISTORY + 2U; k++) {
        size_t n = uplink_mac(f, nwk, addr, fc++, NULL, 0, 0, false);
        CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    }
    CHECK(ns.link[idx].snr_n == 0);
    /* the first ADR uplink: 5 dB at SF12 is 15 dB spare: SF7 (DR5) at once, power unchanged */
    size_t n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(dl.link_adr && dl.adr_dr == 5 && dl.adr_power == 0 && dl.dev_status);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && (d.fctrl & LWC_LW_FCTRL_ADR) != 0U);
    CHECK(d.fopts_len == 6 && d.fopts[0] == 0x03 && d.fopts[1] == 0x50 && d.fopts[2] == 0xFF && d.fopts[3] == 0 &&
          d.fopts[4] == 0x01 && d.fopts[5] == 0x06);
    CHECK(ns.link[idx].adr_pending);

    /* accepted: the device now sends at SF7; 5 dB there is 2.5 dB spare: nothing more */
    static const uint8_t ok[] = {0x03, 0x07, 0x06, 0x00, 0x00};
    rx.sf = 7;
    n = uplink_mac(f, nwk, addr, fc++, ok, sizeof ok, LWC_LW_FCTRL_ADR, false);
    CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(!ns.link[idx].adr_pending && ns.link[idx].tx_power == 0 && ns.link[idx].snr_n == 2);

    /* a strong signal: 15 dB at SF7 is 12.5 dB spare: four power steps */
    rx.snr10 = 150;
    n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(dl.link_adr && dl.adr_dr == 5 && dl.adr_power == 4 && !dl.dev_status);
    /* refused (power): cap the power at index 3, wait a full history, then ask for that */
    static const uint8_t nack[] = {0x03, 0x03};
    n = uplink_mac(f, nwk, addr, fc++, nack, sizeof nack, LWC_LW_FCTRL_ADR, false);
    CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(ns.link[idx].power_cap == 4 && ns.link[idx].tx_power == 0 && ns.link[idx].adr_nacks == 1);
    for (unsigned k = 0; k + 1U < LWC_NS_ADR_HISTORY; k++) {
        n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    }
    n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(dl.link_adr && dl.adr_power == 3);
    /* the downlink was lost: no answer in the next uplink, so wait again */
    n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(!ns.link[idx].adr_pending && ns.link[idx].adr_wait == LWC_NS_ADR_HISTORY - 1U);
    /* accepted after the wait: power changed, so the history starts again */
    for (unsigned k = 0; k < LWC_NS_ADR_HISTORY; k++) {
        n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        (void)lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl);
    }
    CHECK(ns.link[idx].adr_pending && ns.link[idx].pend_power == 3);
    n = uplink_mac(f, nwk, addr, fc++, ok, 2, LWC_LW_FCTRL_ADR, false);
    CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(ns.link[idx].tx_power == 3 && ns.link[idx].snr_n == 1);

    /* the signal drops to -5 dB at SF7 (7.5 dB short: 2 steps up in power), but
     * raising the power waits until the strong entry has left a full history */
    rx.snr10 = -50;
    for (unsigned k = 0; k + 1U < LWC_NS_ADR_HISTORY; k++) {
        n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    }
    n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(dl.link_adr && dl.adr_dr == 5 && dl.adr_power == 1);
    CHECK(lwc_ns_remove(&ns, eui) == LWC_NS_OK);
}

/* A device that accepts a data rate but keeps its own (seen on the desk: an
 * application that fixes the rate) must not get a request on every uplink. */
static void adr_fixed_dr(void)
{
    static const uint64_t eui = 0x78;
    size_t idx = 0, alen = 0;
    uint8_t jr[23], acc[40], nwk[16], app[16], f[64];
    CHECK(lwc_ns_add(&ns, eui, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, "fixed", &idx) == LWC_NS_OK);
    join_request(jr, KEY, eui, 1);
    const uint32_t jn = ns.join_nonce;
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    lwc_lw_session_keys(KEY, jn, 0, 1, nwk, app);
    const uint32_t addr = ns.dev[idx].dev_addr;
    lwc_ns_uplink_t up;
    lwc_ns_dl_t dl;
    lwc_ns_rx_t rx = {.snr10 = 50, .sf = 9, .bw_khz = 125, .now_s = 1};
    uint32_t fc = 0;
    static const uint8_t ok[] = {0x03, 0x07};

    /* 5 dB at SF9 (DR3): 7.5 dB spare, two steps: DR5 */
    size_t n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(dl.link_adr && dl.adr_dr == 5 && ns.link[idx].dr_cap == 0);
    /* accepted, but the answer still comes at SF9: no cap yet, wait a full history */
    n = uplink_mac(f, nwk, addr, fc++, ok, sizeof ok, LWC_LW_FCTRL_ADR, false);
    CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    CHECK(ns.link[idx].dr_cap == 0 && ns.link[idx].dr_kept == 1 && ns.link[idx].adr_wait == LWC_NS_ADR_HISTORY - 1U);
    for (unsigned k = 0; k + 1U < LWC_NS_ADR_HISTORY; k++) {
        n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    }
    /* asked again, accepted again, still at SF9: now cap the rate at DR3 */
    n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
    CHECK(lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl) && dl.link_adr && dl.adr_dr == 5);
    n = uplink_mac(f, nwk, addr, fc++, ok, sizeof ok, LWC_LW_FCTRL_ADR, false);
    const bool again = lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl);
    CHECK(ns.link[idx].dr_cap == 4);
    CHECK(!again || (dl.link_adr && dl.adr_dr == 3)); /* power steps only, at the rate it uses */
    /* from here on: never a higher rate; the power steps down, then it goes quiet */
    unsigned late = 0;
    for (unsigned k = 0; k < 2U * LWC_NS_ADR_HISTORY; k++) {
        const bool ans = ns.link[idx].adr_pending;
        n = uplink_mac(f, nwk, addr, fc++, ans ? ok : NULL, ans ? sizeof ok : 0, LWC_LW_FCTRL_ADR, false);
        if (lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl) && dl.link_adr) {
            CHECK(dl.adr_dr == 3);
            late += k >= LWC_NS_ADR_HISTORY ? 1U : 0U;
        }
    }
    CHECK(late == 0U && ns.link[idx].tx_power == LWC_NS_ADR_MAX_POWER);
    /* capped, the device later sends faster on its own: never asked to slow down */
    rx.sf = 7;
    rx.snr10 = 150; /* 12.5 dB spare: steps it cannot take (rate capped, power at the limit) */
    for (unsigned k = 0; k < 2U * LWC_NS_ADR_HISTORY; k++) {
        n = uplink_mac(f, nwk, addr, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(!lwc_ns_answer(&ns, lwc_ns_uplink(&ns, f, n, &up), &up, &rx, &dl));
    }
    /* a new join clears the cap */
    join_request(jr, KEY, eui, 2);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    CHECK(ns.link[idx].dr_cap == 0);
    CHECK(lwc_ns_remove(&ns, eui) == LWC_NS_OK);
}

/* Receive windows (ADR-0017 step 5): sent in the join-accept, kept per device. */
static void rx_windows(void)
{
    static lwc_ns_t n2;
    lwc_ns_init(&n2);
    CHECK(n2.rx1_delay_s == 1 && n2.rx2_hz == EU->rx2_hz && n2.rx2_dr == 0);
    CHECK(!lwc_ns_set_rx(&n2, 0, 869525000U, 0) && !lwc_ns_set_rx(&n2, 16, 869525000U, 0));
    CHECK(!lwc_ns_set_rx(&n2, 1, 869525000U, 6) && !lwc_ns_set_rx(&n2, 1, 862900000U, 0));
    CHECK(lwc_ns_set_rx(&n2, 3, 869525000U, 3));
    static const uint64_t eui = 0x99;
    size_t idx = 0, alen = 0;
    uint8_t jr[23], acc[40];
    CHECK(lwc_ns_add(&n2, eui, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, "rx", &idx) == LWC_NS_OK);
    join_request(jr, KEY, eui, 7);
    CHECK(lwc_ns_join(&n2, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT && alen == 33);
    CHECK(n2.dev[idx].rx1_delay_s == 3 && n2.dev[idx].rx2_dr == 3 && n2.dev[idx].rx2_hz == 869525000U);
    CHECK(lwc_ns_rx1_delay_us(&n2.dev[idx]) == 3000000U);
    /* the join-accept is AES-decrypted on the server side: encrypting gives the plain text */
    lwc_aes128_t a;
    lwc_aes128_init(&a, KEY);
    uint8_t plain[32];
    lwc_aes128_encrypt(&a, &acc[1], plain);
    lwc_aes128_encrypt(&a, &acc[17], &plain[16]);
    lwc_aes128_wipe(&a);
    CHECK(plain[10] == 0x03 /* DLSettings: RX1DROffset 0, RX2 DR3 */ && plain[11] == 3 /* RxDelay */);
    /* a later change applies to new joins only */
    CHECK(lwc_ns_set_rx(&n2, 1, EU->rx2_hz, 0) && n2.dev[idx].rx1_delay_s == 3);
    lwc_ns_dev_t none;
    memset(&none, 0, sizeof none);
    CHECK(lwc_ns_rx1_delay_us(&none) == 1000000U); /* an unset value reads as 1 s */
}

/* ADR-0024: the dynamic-channel regions. */
static void regions(void)
{
    static lwc_ns_t n2;
    lwc_ns_init(&n2);
    CHECK(n2.region == EU && lwc_ns_ch_mask(&n2) == 0x00FFU); /* 3 defaults + 5 CFList */
    /* fewer CFList channels: fewer bits (a bit for a missing channel makes the device refuse) */
    lwc_plan_t p;
    (void)lwc_plan_default(&p, EU);
    p.multisf[6].enable = false;
    p.multisf[7].enable = false;
    lwc_ns_set_plan(&n2, &p);
    CHECK(lwc_ns_ch_mask(&n2) == 0x003FU);
    CHECK(!lwc_ns_set_region(&n2, lwc_region(LWC_REGION_CN470)) && n2.region == EU); /* ADR-0026 */
    CHECK(!lwc_ns_set_region(&n2, NULL) && !lwc_ns_set_region(NULL, EU) && lwc_ns_ch_mask(NULL) == 0U);
    for (int i = 0; i < (int)LWC_REGIONS; i++) {
        const lwc_region_t *r = lwc_region((lwc_region_id_t)i);
        if (r->fixed) {
            continue;
        }
        lwc_ns_init(&n2);
        CHECK(lwc_ns_set_region(&n2, r) && n2.region == r);
        CHECK(n2.rx1_delay_s == 1U && n2.rx2_hz == r->rx2_hz && n2.rx2_dr == r->rx2_dr);
        CHECK(lwc_ns_ch_mask(&n2) == (r->id == LWC_REGION_EU868 ? 0x00FFU : (uint16_t)((1U << r->n_default) - 1U)));
        CHECK(lwc_ns_rx_ok(r, 1, r->rx2_hz, r->rx2_dr) && !lwc_ns_rx_ok(r, 0, r->rx2_hz, r->rx2_dr));
        CHECK(!lwc_ns_rx_ok(r, 1, r->rx2_hz, 6)); /* DR6: 250 kHz or not defined */
        if (r->id != LWC_REGION_EU868) {
            CHECK(!lwc_ns_rx_ok(r, 1, r->rx2_hz + 200000U, r->rx2_dr)); /* band edges not known: default only */
        }
        /* a join: the region's RX2 data rate in DLSettings, no CFList channels in the default plan */
        size_t idx = 0, alen = 0;
        uint8_t jr[23], acc[40];
        CHECK(lwc_ns_add(&n2, 0x42, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, NULL, &idx) == LWC_NS_OK);
        join_request(jr, KEY, 0x42, 1);
        CHECK(lwc_ns_join(&n2, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT && alen == 33U);
        lwc_aes128_t a;
        lwc_aes128_init(&a, KEY);
        uint8_t plain[32];
        lwc_aes128_encrypt(&a, &acc[1], plain);
        lwc_aes128_encrypt(&a, &acc[17], &plain[16]);
        lwc_aes128_wipe(&a);
        CHECK(plain[10] == r->rx2_dr && plain[11] == 1U);
        CHECK(n2.dev[idx].rx2_hz == r->rx2_hz && n2.dev[idx].rx2_dr == r->rx2_dr);
        if (r->id != LWC_REGION_EU868) {
            static const uint8_t zero[15] = {0};
            CHECK(memcmp(&plain[12], zero, 15) == 0); /* CFList: no extra channels */
        }
        /* sizes: AS923 (TxParamSetupReq) uses the 400 ms dwell sizes */
        CHECK(lwc_ns_max_frm(r, 2) == lwc_region_max_n(r, 2, r->tx_param_setup) && lwc_ns_max_frm(r, 2) > 0U);
    }
    const lwc_region_t *as1 = lwc_region(LWC_REGION_AS923_1);
    CHECK(lwc_ns_max_frm(as1, 2) < lwc_region_max_n(as1, 2, false));
    CHECK(lwc_ns_max_frm(EU, 8) == 0U && lwc_ns_max_frm(NULL, 0) == 0U && lwc_ns_dr(NULL, 7, 125) == -1);
}

/* ADR-0026: US915 and AU915 on one sub-band. Expected values: the reference
 * (core/test/region_ref.inc, brocaar/lorawan/band) and RP002 typed by hand. */
typedef struct {
    const char *region;
    const char *proto;
    uint8_t sb;
    uint8_t nmask;
    uint16_t mask[5];
    uint8_t ncmd;
    struct {
        uint16_t mask;
        uint8_t cntl;
    } cmd[2];
} ref_t;

static const ref_t REF[] = {
#include "region_ref.inc"
};

static const ref_t *ref(const char *region, const char *proto, uint8_t sb)
{
    for (size_t i = 0; i < sizeof REF / sizeof REF[0]; i++) {
        if (strcmp(REF[i].region, region) == 0 && strcmp(REF[i].proto, proto) == 0 && REF[i].sb == sb) {
            return &REF[i];
        }
    }
    return NULL;
}

static lwc_ns_t nf; /* large: keep off the stack */

/* The two LinkADRReq of the sub-band pair in FOpts, against the reference. */
static bool pair_ok(const lwc_lw_data_t *d, const ref_t *rf, uint8_t dr_pw)
{
    if (rf == NULL || rf->ncmd != 2U || d->fopts_len < 10U) {
        return false;
    }
    for (unsigned k = 0; k < 2U; k++) {
        const uint8_t *c = &d->fopts[5U * k];
        if (c[0] != 0x03 || c[1] != dr_pw || (uint16_t)(c[2] | (c[3] << 8)) != rf->cmd[k].mask ||
            c[4] != (uint8_t)(((unsigned)rf->cmd[k].cntl << 4) | 1U)) {
            return false;
        }
    }
    return true;
}

static void fixed_regions(void)
{
    static const struct {
        lwc_region_id_t id;
        uint32_t ch8_hz;  /* uplink channel 8: the first of sub-band 2 (index 1) */
        uint32_t ch72_hz; /* uplink channel 65: its 500 kHz channel */
        uint8_t dr500;    /* the 500 kHz uplink DR (SF8): US915 DR4, AU915 DR6 */
        uint8_t dr_sf10;  /* the uplink DR of SF10 at 125 kHz: US915 DR0, AU915 DR2 */
        uint8_t top;      /* the fastest 125 kHz uplink DR: US915 DR3, AU915 DR5 */
    } T[] = {
        {LWC_REGION_US915, 903900000U, 904600000U, 4, 0, 3},
        {LWC_REGION_AU915, 916800000U, 917500000U, 6, 2, 5},
    };
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {
        const lwc_region_t *r = lwc_region(T[t].id);
        lwc_ns_init(&nf);
        CHECK(lwc_ns_region_ok(r) && lwc_ns_set_region(&nf, r) && nf.region == r && nf.subband == 1);
        CHECK(nf.rx2_hz == 923300000U && nf.rx2_dr == 8 && nf.rx1_delay_s == 1);
        /* RX2: a downlink DR (DR8-13) at 923.3 MHz only */
        CHECK(lwc_ns_rx_ok(r, 1, 923300000U, 8) && lwc_ns_rx_ok(r, 1, 923300000U, 13));
        CHECK(!lwc_ns_rx_ok(r, 1, 923300000U, 0) && !lwc_ns_rx_ok(r, 1, 923900000U, 8) && !lwc_ns_rx_ok(r, 1, 923300000U, 14));

        /* join-accept: 1.0.4 with the reference's CFList (ChMask type 1), 1.0.2 without */
        size_t i4 = 0, i2 = 0, alen = 0;
        uint8_t jr[23], acc[40], plain[32];
        CHECK(lwc_ns_add(&nf, 0x104, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, NULL, &i4) == LWC_NS_OK);
        CHECK(lwc_ns_add(&nf, 0x102, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, NULL, &i2) == LWC_NS_OK);
        join_request(jr, KEY, 0x104, 1);
        uint32_t jn4 = nf.join_nonce;
        CHECK(lwc_ns_join(&nf, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT && alen == 33U);
        lwc_aes128_t a;
        lwc_aes128_init(&a, KEY);
        lwc_aes128_encrypt(&a, &acc[1], plain);
        lwc_aes128_encrypt(&a, &acc[17], &plain[16]);
        const ref_t *rf4 = ref(r->name, "1.0.4", 1), *rf2 = ref(r->name, "1.0.2", 1);
        CHECK(rf4 != NULL && rf2 != NULL && plain[10] == 8 && plain[11] == 1);
        uint8_t want[16] = {0};
        for (unsigned k = 0; rf4 != NULL && k < rf4->nmask; k++) {
            want[2U * k] = (uint8_t)rf4->mask[k];
            want[2U * k + 1U] = (uint8_t)(rf4->mask[k] >> 8);
        }
        want[15] = 1;
        CHECK(memcmp(&plain[12], want, 16) == 0);
        join_request(jr, KEY, 0x102, 1);
        const uint32_t jn2 = nf.join_nonce;
        CHECK(lwc_ns_join(&nf, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT && alen == 17U);
        lwc_aes128_encrypt(&a, &acc[1], plain);
        lwc_aes128_wipe(&a);
        CHECK(plain[10] == 8 && plain[11] == 1);

        /* a plan that is not one sub-band: joins refused, the DevNonce not used up */
        lwc_plan_t p;
        CHECK(lwc_plan_default(&p, r));
        p.multisf[3].enable = false;
        lwc_ns_set_plan(&nf, &p);
        CHECK(nf.subband == -1);
        join_request(jr, KEY, 0x104, 2);
        CHECK(lwc_ns_join(&nf, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_NO_PLAN);
        CHECK(lwc_plan_default(&p, r));
        lwc_ns_set_plan(&nf, &p);
        CHECK(nf.subband == 1);
        jn4 = nf.join_nonce;
        CHECK(lwc_ns_join(&nf, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
        uint8_t nwk4[16], app4[16], nwk2[16], app2[16], f[64];
        lwc_lw_session_keys(KEY, jn4, 0, 2, nwk4, app4);
        lwc_lw_session_keys(KEY, jn2, 0, 1, nwk2, app2);

        /* the first uplink: the sub-band pair, as the reference, with the
         * device's own rate and power (1.0.2) or "keep" (1.0.4: 0xFF) */
        lwc_ns_uplink_t up;
        lwc_ns_dl_t dl;
        lwc_lw_data_t d;
        lwc_ns_rx_t rx = {.snr10 = 50, .sf = 10, .bw_khz = 125, .now_s = 1};
        const uint32_t a2 = nf.dev[i2].dev_addr, a4 = nf.dev[i4].dev_addr;
        size_t n = uplink_mac(f, nwk2, a2, 0, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask && !dl.link_adr);
        CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && pair_ok(&d, rf2, (uint8_t)(T[t].dr_sf10 << 4)));
        CHECK(nf.link[i2].ch_pending && !nf.link[i2].ch_done);
        n = uplink_mac(f, nwk4, a4, 0, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask);
        CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && pair_ok(&d, rf4, 0xFF));
        /* refused (one LinkADRAns per command): sent again after LWC_NS_CH_RETRY (4) uplinks */
        static const uint8_t nack[] = {0x03, 0x06, 0x03, 0x06};
        n = uplink_mac(f, nwk2, a2, 1, nack, sizeof nack, 0, false);
        CHECK(!lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl));
        CHECK(!nf.link[i2].ch_pending && !nf.link[i2].ch_done && nf.link[i2].adr_wait == 0 && nf.link[i2].dr_cap == 0);
        uint32_t fc = 2;
        for (unsigned k = 0; k < 4U; k++) {
            n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
            CHECK(!lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl));
        }
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask);
        /* accepted: done, no more pairs */
        static const uint8_t ack[] = {0x03, 0x07, 0x03, 0x07};
        n = uplink_mac(f, nwk2, a2, fc++, ack, sizeof ack, 0, false);
        CHECK(!lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && nf.link[i2].ch_done);
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, true);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ack && !dl.ch_mask);

        /* ADR after the pair: one LinkADRReq with the sub-band's block mask, up to the
         * fastest 125 kHz DR. 20 dB at SF10 (floor -15 dB) less the 10 dB margin
         * leaves 25 dB: 8 steps, the DR stops at the top 125 kHz DR. */
        rx.snr10 = 200;
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.link_adr && !dl.ch_mask);
        CHECK(dl.adr_dr == T[t].top);
        CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts[0] == 0x03 &&
              (uint16_t)(d.fopts[2] | (d.fopts[3] << 8)) == rf2->cmd[1].mask && d.fopts[4] == ((rf2->cmd[1].cntl << 4) | 1));
        /* ADR folded into the pair: a 1.0.4 device that has not accepted the pair
         * yet, on its first ADR uplink (session state as after a restart) */
        memset(&nf.link[i4], 0, sizeof nf.link[i4]);
        n = uplink_mac(f, nwk4, a4, 6, NULL, 0, LWC_LW_FCTRL_ADR, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl));
        /* 25 dB spare = 8 steps: 3 to the top DR, the other 5 lower the power (index 5, the cap) */
        CHECK(dl.ch_mask && dl.link_adr && dl.adr_dr == T[t].top && dl.adr_power == 5 &&
              lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && pair_ok(&d, rf4, (uint8_t)(((unsigned)T[t].top << 4) | 5U)));
        CHECK(nf.link[i4].ch_pending && nf.link[i4].adr_pending);
        /* no room for both commands (LinkCheckAns 3 B + DeviceTimeAns 6 B): no half pair */
        lwc_ns_link_t keep = nf.link[i4];
        nf.link[i4].ch_pending = false;
        nf.link[i4].ch_wait = 0;
        static const uint8_t lcr[] = {0x02, 0x0D}; /* LinkCheckReq, DeviceTimeReq */
        lwc_ns_rx_t rxt = rx;
        rxt.time_ok = true;
        rxt.gps_s = 1400000000U;
        n = uplink_mac(f, nwk4, a4, 7, lcr, sizeof lcr, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rxt, &dl) && dl.link_check && dl.device_time &&
              !dl.ch_mask && !nf.link[i4].ch_pending);
        nf.link[i4] = keep;

        /* back-off: each refusal doubles the wait (4, then 8 uplinks) */
        memset(&nf.link[i2], 0, sizeof nf.link[i2]);
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask);
        n = uplink_mac(f, nwk2, a2, fc++, nack, sizeof nack, 0, false);
        (void)lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl);
        CHECK(nf.link[i2].ch_wait == 4 && nf.link[i2].ch_tries == 1);
        for (unsigned k = 0; k < 4U; k++) {
            n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
            (void)lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl);
        }
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask);
        n = uplink_mac(f, nwk2, a2, fc++, nack, sizeof nack, 0, false);
        (void)lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl);
        CHECK(nf.link[i2].ch_wait == 8 && nf.link[i2].ch_tries == 2);
        n = uplink_mac(f, nwk2, a2, fc++, ack, sizeof ack, 0, false); /* a late answer is not taken */
        (void)lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl);
        CHECK(!nf.link[i2].ch_done);
        nf.link[i2].ch_done = true;

        /* another sub-band on the radio: every device gets the new pair (reference masks of sub-band 1) */
        CHECK(lwc_plan_subband(&p, r, 0));
        lwc_ns_set_plan(&nf, &p);
        CHECK(nf.subband == 0 && !nf.link[i2].ch_done && nf.link[i2].ch_wait == 0);
        n = uplink_mac(f, nwk2, a2, fc++, NULL, 0, 0, false);
        CHECK(lwc_ns_answer(&nf, lwc_ns_uplink(&nf, f, n, &up), &up, &rx, &dl) && dl.ch_mask);
        CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK &&
              pair_ok(&d, ref(r->name, "1.0.2", 0), (uint8_t)(T[t].dr_sf10 << 4)));
        lwc_ns_set_plan(&nf, &p); /* the same sub-band again: nothing reset */
        CHECK(nf.link[i2].ch_pending);

        /* downlink timing: RX1 on downlink channel (uplink % 8) at 500 kHz, RX2 923.3 MHz DR8 (SF12, 500 kHz) */
        lwc_txpk_t tx;
        lwc_ns_downlink(r, false, 1000, T[t].ch8_hz, 10, 125, LWC_NS_RX1_DELAY_US, 923300000U, 8, 14, NULL, 0, &tx);
        CHECK(tx.freq_hz == 923300000U && tx.sf == 10 && tx.bw_khz == 500 && tx.tmst == 1000 + 1000000U);
        lwc_ns_downlink(r, false, 0, T[t].ch8_hz + 200000U, 7, 125, LWC_NS_RX1_DELAY_US, 923300000U, 8, 14, NULL, 0, &tx);
        CHECK(tx.freq_hz == 923900000U && tx.sf == 7 && tx.bw_khz == 500);
        lwc_ns_downlink(r, false, 0, T[t].ch72_hz, 8, 500, LWC_NS_RX1_DELAY_US, 923300000U, 8, 14, NULL, 0, &tx);
        CHECK(tx.freq_hz == 923900000U && tx.sf == 7 && tx.bw_khz == 500); /* DR(SF8/500) -> DR13 */
        lwc_ns_downlink(r, true, 0, T[t].ch8_hz, 10, 125, LWC_NS_RX1_DELAY_US, 923300000U, 8, 14, NULL, 0, &tx);
        CHECK(tx.freq_hz == 923300000U && tx.sf == 12 && tx.bw_khz == 500 && tx.tmst == 2000000U);
        lwc_ns_downlink(r, true, 0, 0, 0, 0, 0, 923300000U, 3, 14, NULL, 0, &tx); /* not a downlink DR: DR8 */
        CHECK(tx.sf == 12 && tx.bw_khz == 500);
        lwc_ns_downlink_at(r, 5000000U, 923300000U, 10, 14, NULL, 0, &tx); /* class C */
        CHECK(tx.tmst == 5000000U && tx.freq_hz == 923300000U && tx.sf == 10 && tx.bw_khz == 500);
        CHECK(lwc_ns_dr(r, 8, 500) == T[t].dr500);

        /* sizes: every RX1 DR an uplink can lead to, and every RX2 DR, has room */
        for (int dr = 0; dr < 8; dr++) {
            if (r->dr[dr].kind != LWC_DR_LORA) {
                continue;
            }
            CHECK(lwc_ns_max_frm(r, lwc_region_rx1_dr(r, dr, 0, false)) > 0U);
        }
        for (uint8_t dr = 8; dr <= 13U; dr++) {
            CHECK(lwc_ns_max_frm(r, dr) > 0U);
        }
    }
    const lwc_region_t *cn = lwc_region(LWC_REGION_CN470);
    lwc_ns_init(&nf);
    CHECK(!lwc_ns_region_ok(cn) && !lwc_ns_set_region(&nf, cn) && nf.region == EU && !lwc_ns_rx_ok(cn, 1, cn->rx2_hz, 0));
    CHECK(!lwc_ns_region_ok(NULL) && lwc_ns_region_ok(EU));
}

/* ADR-0027: TxParamSetupReq with the region's defaults (uplink dwell on,
 * downlink dwell off, the region's MaxEIRP), sent until answered. */
static lwc_ns_t nt; /* large: keep off the stack */

static void tx_param(void)
{
    uint8_t jr[23], acc[40], f[64], nwk[16], app[16];
    size_t idx = 0, alen = 0, n;
    lwc_ns_uplink_t up;
    lwc_ns_dl_t dl;
    lwc_lw_data_t d;
    lwc_ns_rx_t rx = {.snr10 = 50, .sf = 10, .bw_khz = 125, .now_s = 1};
    static const uint8_t tpa[] = {0x09}; /* TxParamSetupAns */

    /* AS923-1, a 1.0.2 device (TxParamSetupReq is in 1.0.2 for AS923) */
    const lwc_region_t *as1 = lwc_region(LWC_REGION_AS923_1);
    lwc_ns_init(&nt);
    CHECK(lwc_ns_set_region(&nt, as1));
    CHECK(lwc_ns_add(&nt, 0x923, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, NULL, &idx) == LWC_NS_OK);
    join_request(jr, KEY, 0x923, 1);
    const uint32_t jn = nt.join_nonce;
    CHECK(lwc_ns_join(&nt, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    lwc_lw_session_keys(KEY, jn, 0, 1, nwk, app);
    const uint32_t a = nt.dev[idx].dev_addr;
    const size_t dwell2 = lwc_region_max_n(as1, 2, true), free2 = lwc_region_max_n(as1, 2, false);
    CHECK(dwell2 < free2 && lwc_ns_dev_max_frm(&nt, idx, 2) == dwell2); /* not known yet: the smaller size */
    uint32_t fc = 0;
    n = uplink_mac(f, nwk, a, fc++, NULL, 0, 0, false);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && dl.tx_param && !dl.ch_mask);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts_len >= 2U && d.fopts[0] == 0x09 &&
          d.fopts[1] == 0x15); /* uplink dwell, MaxEIRP 16 dBm (index 5) */
    CHECK(nt.link[idx].tp_pending && !nt.link[idx].tp_done);
    /* no answer: sent again after LWC_NS_CH_RETRY (4) uplinks */
    n = uplink_mac(f, nwk, a, fc++, NULL, 0, 0, false);
    CHECK(!lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl));
    CHECK(!nt.link[idx].tp_pending && nt.link[idx].tp_wait == 4 && nt.link[idx].tp_tries == 1);
    for (unsigned k = 0; k < 4U; k++) {
        n = uplink_mac(f, nwk, a, fc++, NULL, 0, 0, false);
        CHECK(!lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl));
    }
    n = uplink_mac(f, nwk, a, fc++, NULL, 0, 0, false);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && dl.tx_param);
    /* answered: downlink dwell off, the larger size; not sent again */
    n = uplink_mac(f, nwk, a, fc++, tpa, sizeof tpa, 0, false);
    CHECK(!lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && nt.link[idx].tp_done);
    CHECK(lwc_ns_dev_max_frm(&nt, idx, 2) == free2 && lwc_ns_max_frm(as1, 2) == dwell2);
    CHECK(lwc_ns_dev_max_frm(&nt, idx, 14) == 0U && lwc_ns_dev_max_frm(NULL, 0, 2) == 0U &&
          lwc_ns_dev_max_frm(&nt, LWC_NS_MAX_DEVICES, 2) == 0U);
    n = uplink_mac(f, nwk, a, fc++, NULL, 0, 0, true);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && dl.ack && !dl.tx_param);
    /* a new session: unknown again */
    join_request(jr, KEY, 0x923, 2);
    CHECK(lwc_ns_join(&nt, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    CHECK(!nt.link[idx].tp_done && lwc_ns_dev_max_frm(&nt, idx, 2) == dwell2);

    /* EU868 and US915: no TxParamSetupReq */
    static const lwc_region_id_t no[] = {LWC_REGION_EU868, LWC_REGION_US915};
    for (size_t t = 0; t < sizeof no / sizeof no[0]; t++) {
        lwc_ns_init(&nt);
        CHECK(lwc_ns_set_region(&nt, lwc_region(no[t])));
        CHECK(lwc_ns_add(&nt, 0x868, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, NULL, &idx) == LWC_NS_OK);
        join_request(jr, KEY, 0x868, 1);
        const uint32_t j = nt.join_nonce;
        CHECK(lwc_ns_join(&nt, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
        lwc_lw_session_keys(KEY, j, 0, 1, nwk, app);
        nt.link[idx].ch_done = true;
        n = uplink_mac(f, nwk, nt.dev[idx].dev_addr, 0, NULL, 0, 0, false);
        CHECK(!lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && !dl.tx_param);
    }

    /* AU915: 1.0.4 devices only, after the sub-band pair, never beside it */
    const lwc_region_t *au = lwc_region(LWC_REGION_AU915);
    lwc_ns_init(&nt);
    CHECK(lwc_ns_set_region(&nt, au) && nt.subband == 1);
    size_t i4 = 0, i2 = 0;
    CHECK(lwc_ns_add(&nt, 0x104, JOIN_EUI, KEY, LWC_NS_MAC_1_0_4, NULL, &i4) == LWC_NS_OK);
    CHECK(lwc_ns_add(&nt, 0x102, JOIN_EUI, KEY, LWC_NS_MAC_1_0_2, NULL, &i2) == LWC_NS_OK);
    uint8_t nwk2[16], app2[16];
    join_request(jr, KEY, 0x104, 1);
    uint32_t j4 = nt.join_nonce;
    CHECK(lwc_ns_join(&nt, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    join_request(jr, KEY, 0x102, 1);
    const uint32_t j2 = nt.join_nonce;
    CHECK(lwc_ns_join(&nt, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    lwc_lw_session_keys(KEY, j4, 0, 1, nwk, app);
    lwc_lw_session_keys(KEY, j2, 0, 1, nwk2, app2);
    static const uint8_t ack[] = {0x03, 0x07, 0x03, 0x07};
    const uint32_t a4 = nt.dev[i4].dev_addr, a2 = nt.dev[i2].dev_addr;
    n = uplink_mac(f, nwk, a4, 0, NULL, 0, 0, false);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && dl.ch_mask && !dl.tx_param);
    n = uplink_mac(f, nwk, a4, 1, ack, sizeof ack, 0, false);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && nt.link[i4].ch_done && dl.tx_param);
    CHECK(lwc_lw_data_parse(dl.frame, dl.len, &d) == LWC_LW_OK && d.fopts[0] == 0x09 &&
          d.fopts[1] == 0x1D); /* uplink dwell, MaxEIRP 30 dBm (index 13) */
    n = uplink_mac(f, nwk2, a2, 0, NULL, 0, 0, false);
    CHECK(lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && dl.ch_mask && !dl.tx_param);
    n = uplink_mac(f, nwk2, a2, 1, ack, sizeof ack, 0, false);
    CHECK(!lwc_ns_answer(&nt, lwc_ns_uplink(&nt, f, n, &up), &up, &rx, &dl) && nt.link[i2].ch_done &&
          !nt.link[i2].tp_pending);
}

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
    CHECK(plain[11] == 0x00 && plain[12] == 1); /* defaults: RX2 DR0, RX1 delay 1 s */
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
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_RETRANS && up.len == 0); /* the same frame again */
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
    n = uplink(f, nwk, app, addr, 0x10000, 2, "old", false); /* older counter: read as the next epoch */
    CHECK(lwc_ns_uplink(&ns, f, n, &up) == LWC_NS_UP_BAD_MIC);

    answers(idx, addr, nwk, app, 0x10002);
    app_downlinks(idx, addr, nwk, app, 0x10100);
    CHECK(ns.link[idx].has_status);
    class_c(idx, addr, nwk, app);

    /* rejoin keeps the address, resets counters */
    join_request(jr, KEY, DEV_EUI, 6);
    CHECK(lwc_ns_join(&ns, jr, 23, acc, sizeof acc, &alen, NULL) == LWC_NS_JOIN_ACCEPT);
    CHECK(ns.dev[idx].dev_addr == addr && !ns.dev[idx].has_up && !ns.link[idx].has_status);

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
    ns.link[i2].has_status = true;
    CHECK(lwc_ns_remove(&ns, 0x42) == LWC_NS_OK && !ns.dev[i2].used && ns.dev[i2].app_key[0] == 0);
    CHECK(!ns.link[i2].has_status);
    CHECK(lwc_ns_remove(&ns, 0x42) == LWC_NS_ERR_NOT_FOUND);

    /* table full */
    lwc_ns_t full;
    lwc_ns_init(&full);
    for (uint64_t e = 1; e <= LWC_NS_MAX_DEVICES; e++) {
        CHECK(lwc_ns_add(&full, e, 0, KEY, LWC_NS_MAC_1_0_4, NULL, NULL) == LWC_NS_OK);
    }
    CHECK(lwc_ns_add(&full, 999, 0, KEY, LWC_NS_MAC_1_0_4, NULL, NULL) == LWC_NS_ERR_FULL);

    adr_step();
    adr_flow();
    adr_fixed_dr();
    fixed_regions();
    tx_param();

    /* routing */
    CHECK(lwc_ns_forward(LWC_MODE_TTN, true) && lwc_ns_forward(LWC_MODE_TTN, false));
    CHECK(!lwc_ns_forward(LWC_MODE_STANDALONE, true) && !lwc_ns_forward(LWC_MODE_STANDALONE, false));
    CHECK(!lwc_ns_forward(LWC_MODE_HYBRID, true) && lwc_ns_forward(LWC_MODE_HYBRID, false));

    rx_windows();
    regions();

    /* downlink timing */
    lwc_txpk_t tx;
    lwc_ns_downlink(EU, false, 1000, 868100000, 9, 125, LWC_NS_JOIN_DELAY_US, EU->rx2_hz, 0, 13, acc, 33, &tx);
    CHECK(tx.tmst == 5001000U && tx.freq_hz == 868100000U && tx.sf == 9 && tx.ipol && tx.ncrc && tx.size == 33);
    lwc_ns_downlink(EU, true, 0xFFFFFFFFU, 868100000, 9, 125, LWC_NS_RX1_DELAY_US, EU->rx2_hz, 0, 13, NULL, 0, &tx);
    CHECK(tx.tmst == 1999999U && tx.freq_hz == EU->rx2_hz && tx.sf == 12 && tx.bw_khz == 125);
    lwc_ns_downlink(EU, true, 0, 868100000, 9, 125, 3000000U, 869525000U, 3, 13, NULL, 0, &tx);
    CHECK(tx.tmst == 4000000U && tx.freq_hz == 869525000U && tx.sf == 9); /* RX2 at DR3 */

    if (failures != 0) {
        fprintf(stderr, "test_ns: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ns: all passed\n");
    return 0;
}
