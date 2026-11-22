/* libFuzzer target: clang -fsanitize=fuzzer,address,undefined */
#include "lwc/txpk.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_txpk_t t;
    if (lwc_txpk_parse((const char *)data, size, &t) == LWC_TXPK_OK && t.size > sizeof t.payload) {
        __builtin_trap();
    }
    return 0;
}
