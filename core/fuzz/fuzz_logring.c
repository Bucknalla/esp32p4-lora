/* libFuzzer target: retained log memory is untrusted after a reset. */
#include "lwc/logring.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static _Alignas(4) uint8_t mem[sizeof(lwc_logring_hdr_t) + 256];
    const size_t n = size < sizeof mem ? size : sizeof mem;
    memset(mem, 0, sizeof mem);
    memcpy(mem, data, n);
    lwc_logring_t r;
    (void)lwc_logring_attach(&r, mem, sizeof mem);
    char out[300];
    const size_t len = lwc_logring_tail(&r, out, sizeof out);
    if (len >= sizeof out || out[len] != '\0') {
        __builtin_trap();
    }
    /* Remaining input as log text. */
    if (size > n) {
        lwc_logring_write(&r, (const char *)data + n, size - n);
        (void)lwc_logring_tail(&r, out, 17);
    }
    return 0;
}
