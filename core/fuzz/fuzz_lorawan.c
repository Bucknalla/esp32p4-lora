/* libFuzzer target: LoRaWAN frames come from the air and are untrusted. */
#include "lwc/lorawan.h"

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
    return 0;
}
