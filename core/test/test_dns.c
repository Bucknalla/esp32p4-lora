#include "lwc/dns.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Query for captive.apple.com, type `t`, RD set. */
static size_t query(uint8_t *b, uint16_t t)
{
    static const uint8_t h[] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    static const uint8_t n[] = {7, 'c', 'a', 'p', 't', 'i', 'v', 'e', 5, 'a', 'p', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0};
    memcpy(b, h, sizeof h);
    memcpy(b + sizeof h, n, sizeof n);
    size_t p = sizeof h + sizeof n;
    b[p++] = (uint8_t)(t >> 8);
    b[p++] = (uint8_t)t;
    b[p++] = 0;
    b[p++] = 1;
    return p;
}

int main(void)
{
    const uint8_t ip[4] = {192, 168, 4, 1};
    uint8_t q[64], r[128];
    size_t n = query(q, 1);
    size_t m = lwc_dns_captive_reply(q, n, ip, r, sizeof r);
    CHECK(m == n + 16U);
    CHECK(r[0] == 0x12 && r[1] == 0x34 && (r[2] & 0x80) && (r[2] & 0x01) && r[3] == 0x80);
    CHECK(r[7] == 1 && memcmp(&r[m - 4], ip, 4) == 0 && r[n] == 0xC0 && r[n + 1] == 0x0C);
    /* AAAA: empty NOERROR answer */
    n = query(q, 28);
    m = lwc_dns_captive_reply(q, n, ip, r, sizeof r);
    CHECK(m == n && r[7] == 0 && r[3] == 0x80);
    /* rejects */
    n = query(q, 1);
    CHECK(lwc_dns_captive_reply(q, n, ip, r, n + 15U) == 0);    /* too small */
    CHECK(lwc_dns_captive_reply(q, n - 1U, ip, r, sizeof r) == 0); /* cut */
    q[2] |= 0x80;                                                /* a response */
    CHECK(lwc_dns_captive_reply(q, n, ip, r, sizeof r) == 0);
    n = query(q, 1);
    q[5] = 2;                                                    /* two questions */
    CHECK(lwc_dns_captive_reply(q, n, ip, r, sizeof r) == 0);
    n = query(q, 1);
    q[12] = 0xC0;                                                /* pointer in a query */
    CHECK(lwc_dns_captive_reply(q, n, ip, r, sizeof r) == 0);
    CHECK(lwc_dns_captive_reply(q, 5, ip, r, sizeof r) == 0);
    CHECK(lwc_dns_captive_reply(NULL, 30, ip, r, sizeof r) == 0);
    if (failures != 0) {
        fprintf(stderr, "test_dns: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_dns: all passed\n");
    return 0;
}
