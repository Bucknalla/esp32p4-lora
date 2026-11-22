/* libFuzzer target: clang -fsanitize=fuzzer,address,undefined */
#include "lwc/console_cmd.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lwc_cmd_t c;
    if (lwc_cmd_parse((const char *)data, size, &c) == LWC_CMD_OK && c.kind == LWC_CMD_WIFI_SET) {
        /* Outputs must be NUL-terminated within their buffers. */
        if (memchr(c.ssid, '\0', sizeof c.ssid) == NULL || memchr(c.pass, '\0', sizeof c.pass) == NULL) {
            __builtin_trap();
        }
    }
    lwc_cmd_wipe(&c);
    return 0;
}
