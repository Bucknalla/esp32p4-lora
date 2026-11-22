/* libFuzzer target: DNS queries on the setup access point are untrusted. */
#include "lwc/dns.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t ip[4] = {192, 168, 4, 1};
    uint8_t out[600];
    const size_t n = lwc_dns_captive_reply(data, size, ip, out, sizeof out);
    if (n > sizeof out || (n != 0 && n < 16)) {
        __builtin_trap();
    }
    return 0;
}
