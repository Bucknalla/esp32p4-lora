/* libFuzzer target: clang -fsanitize=fuzzer,address,undefined */
#include "lwc/frame.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_frame_t fr;
    if (lwc_frame_parse(data, size, &fr) == LWC_FRAME_OK &&
        fr.mtype != LWC_MTYPE_JOIN_REQUEST) {
        /* Touch every byte the parser claims is in bounds. */
        volatile uint8_t sink = 0;
        for (uint8_t i = 0; i < fr.u.data.fopts_len; i++) {
            sink ^= fr.u.data.fopts[i];
        }
        for (uint8_t i = 0; i < fr.u.data.payload_len; i++) {
            sink ^= fr.u.data.payload[i];
        }
        (void)sink;
    }
    return 0;
}
