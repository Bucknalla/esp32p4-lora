/* libFuzzer target: LoRaWAN frames come from the air and are untrusted. */
#include "lwc/lorawan.h"
#include "lwc/mac.h"
#include "lwc/ns.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    lwc_lw_join_req_t jr;
    if (lwc_lw_join_req_parse(data, size, &jr) == LWC_LW_OK) {
        (void)lwc_lw_join_req_mic_ok(key, data, size);
    }
    lwc_lw_data_t d;
    if (lwc_lw_data_parse(data, size, &d) == LWC_LW_OK) {
        if ((size_t)(d.frm - data) + d.frm_len + 4U > size && d.has_port) {
            __builtin_trap(); /* payload must end before the MIC */
        }
        const bool down = d.mtype == LWC_LW_UNCONFIRMED_DOWN || d.mtype == LWC_LW_CONFIRMED_DOWN;
        (void)lwc_lw_data_mic_ok(key, down, d.dev_addr, lwc_lw_fcnt32(0, d.fcnt16), data, size);
        uint8_t out[256];
        if (d.has_port && d.frm_len <= sizeof out) {
            lwc_lw_crypt(key, down, d.dev_addr, d.fcnt16, d.frm, d.frm_len, out);
        }
    }
    /* the network server on the same bytes: one device, joined on the first run */
    static lwc_ns_t ns;
    static int ready;
    if (!ready) {
        lwc_ns_init(&ns);
        (void)lwc_ns_add(&ns, 0x0202020202020202ULL, 0x0101010101010101ULL, key, LWC_NS_MAC_1_0_2, NULL, NULL);
        ns.dev[0].joined = true;
        ns.dev[0].dev_addr = 0x01020304U;
        ready = 1;
    }
    uint8_t acc[40];
    size_t alen;
    (void)lwc_ns_join(&ns, data, size, acc, sizeof acc, &alen, NULL);
    static lwc_ns_uplink_t up;
    const lwc_ns_up_res_t r = lwc_ns_uplink(&ns, data, size, &up);
    if (r == LWC_NS_UP_OK && up.len > sizeof up.payload) {
        __builtin_trap();
    }
    if (r == LWC_NS_UP_OK || r == LWC_NS_UP_RETRANS) {
        static lwc_ns_dl_t dl;
        const lwc_ns_rx_t rx = {.snr10 = (int16_t)((int)(size % 600U) - 300), .sf = (uint8_t)(7U + size % 6U),
                                .bw_khz = 125, .time_ok = (size & 1U) != 0U, .gps_s = 1,
                                .now_s = (uint32_t)size};
        if (lwc_ns_answer(&ns, r, &up, &rx, &dl) && (dl.len < 12U || dl.len > 8U + 15U + 4U)) {
            __builtin_trap(); /* FOpts only: 12..27 bytes */
        }
    }
    /* The answer path on fuzzed MAC commands (a real uplink needs a valid MIC,
     * which fuzzing cannot forge): an accepted uplink from device 0 whose
     * FOpts (or port-0 payload) are the input bytes. */
    if (size >= 2U) {
        static lwc_ns_uplink_t fu;
        memset(&fu, 0, sizeof fu);
        fu.idx = 0;
        fu.confirmed = (data[0] & 1U) != 0U;
        fu.adr = (data[0] & 2U) != 0U;
        fu.adr_ack_req = (data[0] & 4U) != 0U;
        const uint8_t *mac = &data[2];
        const size_t mlen = size - 2U;
        if ((data[0] & 8U) != 0U) {
            fu.has_port = true;
            fu.port = 0;
            fu.len = mlen < sizeof fu.payload ? mlen : sizeof fu.payload;
            memcpy(fu.payload, mac, fu.len);
        } else {
            fu.fopts_len = mlen < sizeof fu.fopts ? mlen : sizeof fu.fopts;
            memcpy(fu.fopts, mac, fu.fopts_len);
        }
        const lwc_ns_rx_t frx = {.snr10 = (int16_t)((int)data[1] * 2 - 250), .sf = (uint8_t)(7U + data[1] % 6U),
                                 .bw_khz = (data[0] & 16U) != 0U ? 250U : 125U, .time_ok = (data[0] & 32U) != 0U,
                                 .gps_s = 1, .now_s = (uint32_t)size * 1000U};
        static lwc_ns_dl_t fdl;
        if (lwc_ns_answer(&ns, (data[0] & 64U) != 0U ? LWC_NS_UP_RETRANS : LWC_NS_UP_OK, &fu, &frx, &fdl)) {
            lwc_lw_data_t dd;
            if (fdl.len < 12U || fdl.len > 8U + 15U + 4U || lwc_lw_data_parse(fdl.frame, fdl.len, &dd) != LWC_LW_OK ||
                !lwc_lw_data_mic_ok(ns.dev[0].nwk_s_key, true, dd.dev_addr, fdl.fcnt, fdl.frame, fdl.len)) {
                __builtin_trap(); /* a well-formed, authentic downlink with FOpts only */
            }
        }
        if (ns.link[0].tx_power > LWC_NS_ADR_MAX_POWER || ns.link[0].snr_n > LWC_NS_ADR_HISTORY) {
            __builtin_trap();
        }
    }
    /* MAC commands as they come in FOpts or on port 0 */
    lwc_mac_up_t m;
    const size_t cmds = lwc_mac_parse_up(data, size, &m);
    if (cmds > size || (m.link_adr_ans == 0U && m.link_adr_status != 0U)) {
        __builtin_trap();
    }
    return 0;
}
