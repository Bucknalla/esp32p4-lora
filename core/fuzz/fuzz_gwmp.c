/* libFuzzer target: clang -fsanitize=fuzzer,address,undefined */
#include "lwc/gwmp.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_gwmp_down_t d;
    if (lwc_gwmp_parse_down(data, size, &d) == LWC_GWMP_OK && d.json != NULL) {
        volatile char sink = 0;
        for (size_t i = 0; i < d.json_len; i++) {
            sink ^= d.json[i];
        }
        (void)sink;
    }
    return 0;
}
