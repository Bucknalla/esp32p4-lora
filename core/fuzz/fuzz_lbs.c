/* libFuzzer target: Basics Station messages from the server are untrusted;
 * every uplink message must carry its frame exactly. */
#include "lwc/lbs.h"

#include <stdint.h>
#include <string.h>

#include "../test/lbs_rebuild.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *json = (const char *)data;
    /* server to gateway */
    lwc_lbs_msg_t t;
    (void)lwc_lbs_msgtype(json, size, &t);
    static lwc_lbs_dnmsg_t d;
    if (lwc_lbs_parse_dnmsg(json, size, &d) == LWC_LBS_OK) {
        if (d.len == 0U || d.len > sizeof d.pdu || (!d.rx1 && !d.rx2) || (!d.asap && d.xtime <= 0) || d.rx_delay > 15U ||
            (d.asap && (d.dclass != 2U || d.rx1 || !d.rx2))) {
            __builtin_trap();
        }
        if (d.asap) {
            static lwc_txpk_t na, nb;
            if (lwc_lbs_dn_txpk_now(&d, 0xFFFFF000U, &na, &nb) == LWC_LBS_OK &&
                (na.size != d.len || nb.size != d.len || nb.tmst - na.tmst != LWC_LBS_ASAP_RETRY_US)) {
                __builtin_trap();
            }
        }
        static lwc_txpk_t a, b;
        const uint8_t sess = (uint8_t)((uint64_t)d.xtime >> 48);
        if (lwc_lbs_dn_txpk(&d, sess, &a, &b) == LWC_LBS_OK &&
            (a.size != d.len || (b.size != 0U && b.size != d.len) || a.sf < 7U || a.sf > 12U)) {
            __builtin_trap();
        }
    }
    lwc_lbs_router_config_t rc;
    (void)lwc_lbs_parse_router_config(json, size, &rc);

    /* gateway to server: the bytes as a received frame */
    static char out[1400];
    size_t n = 0;
    if (size >= 2U) {
        const lwc_lbs_rx_t rx = {.sf = (uint8_t)(7U + data[0] % 6U), .bw_khz = 125, .freq_hz = 868100000,
                                 .xtime = (int64_t)data[1] << 40, .rssi = -100.0f, .snr = 5.5f,
                                 .unix_us = (int64_t)size * 1000003};
        if (lwc_lbs_uplink(out, sizeof out, data, size, &rx, &n) == LWC_LBS_OK) {
            static uint8_t back[255];
            if (n >= sizeof out || size > 255U || lbs_rebuild(out, n, back, sizeof back) != size ||
                memcmp(back, data, size) != 0) {
                __builtin_trap();
            }
        }
    }
    /* the clock never leaves its session */
    lwc_lbs_clock_t c;
    lwc_lbs_clock_init(&c, size > 0U ? data[0] : 0U);
    for (size_t i = 0; i + 4U <= size && i < 64U; i += 4U) {
        uint32_t tm;
        memcpy(&tm, &data[i], 4);
        const int64_t x = lwc_lbs_xtime(&c, tm);
        if ((uint8_t)((uint64_t)x >> 48) != c.sess || x < 0 || c.sess == 0U) {
            __builtin_trap();
        }
    }
    return 0;
}
