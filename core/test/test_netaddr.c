#include "lwc/netaddr.h"
#include "lwc/supervisor.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static bool v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t x[4] = {a, b, c, d};
    return lwc_ip_is_lan(x, 4);
}

static bool mapped(uint8_t a, uint8_t b)
{
    const uint8_t x[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, a, b, 1, 2};
    return lwc_ip_is_lan(x, 16);
}

int main(void)
{
    CHECK(v4(10, 0, 0, 1) && v4(192, 168, 3, 7) && v4(169, 254, 1, 1) && v4(127, 0, 0, 1));
    CHECK(v4(172, 16, 0, 1) && v4(172, 31, 255, 255));
    CHECK(!v4(172, 15, 255, 255) && !v4(172, 32, 0, 0));
    CHECK(!v4(8, 8, 8, 8) && !v4(192, 169, 0, 1) && !v4(11, 0, 0, 1) && !v4(100, 64, 0, 1));
    CHECK(mapped(192, 168) && !mapped(8, 8));
    const uint8_t ula[16] = {0xFD, 0x12};
    const uint8_t ll[16] = {0xFE, 0x80};
    const uint8_t feco[16] = {0xFE, 0xC0}; /* old site-local: not accepted */
    const uint8_t global[16] = {0x20, 0x01, 0x0D, 0xB8};
    const uint8_t loop[16] = {[15] = 1};
    const uint8_t any[16] = {0};
    CHECK(lwc_ip_is_lan(ula, 16) && lwc_ip_is_lan(ll, 16) && lwc_ip_is_lan(loop, 16));
    CHECK(!lwc_ip_is_lan(feco, 16) && !lwc_ip_is_lan(global, 16) && !lwc_ip_is_lan(any, 16));
    CHECK(!lwc_ip_is_lan(ula, 8) && !lwc_ip_is_lan(NULL, 4));

    /* reboot-loop guard: abort for runs 0..4, only log from 5 */
    CHECK(lwc_sv_should_abort(0, 5) && lwc_sv_should_abort(4, 5));
    CHECK(!lwc_sv_should_abort(5, 5) && !lwc_sv_should_abort(99, 5));

    if (failures != 0) {
        fprintf(stderr, "test_netaddr: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_netaddr: all passed\n");
    return 0;
}
