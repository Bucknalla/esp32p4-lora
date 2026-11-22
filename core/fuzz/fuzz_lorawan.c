/* libFuzzer target: LoRaWAN frames come from the air and are untrusted. */
#include "lwc/lorawan.h"
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
    if (lwc_ns_uplink(&ns, data, size, &up) == LWC_NS_UP_OK && up.len > sizeof up.payload) {
        __builtin_trap();
    }
    return 0;
}
