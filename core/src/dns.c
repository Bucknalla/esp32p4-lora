#include "lwc/dns.h"

#include <string.h>

#define HDR 12U

size_t lwc_dns_captive_reply(const uint8_t *q, size_t len, const uint8_t ip[4], uint8_t *out,
                             size_t cap)
{
    if (q == NULL || ip == NULL || out == NULL || len < HDR) {
        return 0;
    }
    const uint16_t flags = (uint16_t)((q[2] << 8) | q[3]);
    const uint16_t qd = (uint16_t)((q[4] << 8) | q[5]);
    if ((flags & 0x8000U) != 0U || (flags & 0x7800U) != 0U || qd != 1U) {
        return 0; /* a response, not a standard query, or not one question */
    }
    /* Walk the name: labels of 1..63 bytes, at most 255 bytes, no pointers. */
    size_t p = HDR, name = 0;
    for (;;) {
        if (p >= len) {
            return 0;
        }
        const uint8_t l = q[p];
        if (l == 0U) {
            p++;
            break;
        }
        if (l > 63U || (name += l + 1U) > 255U) {
            return 0;
        }
        p += 1U + l;
    }
    if (p + 4U > len) {
        return 0;
    }
    const uint16_t qtype = (uint16_t)((q[p] << 8) | q[p + 1U]);
    const size_t qend = p + 4U; /* type + class */
    const int answer = qtype == 1U || qtype == 255U; /* A or ANY */
    const size_t total = qend + (answer ? 16U : 0U);
    if (total > cap) {
        return 0;
    }
    memcpy(out, q, qend);              /* header + question */
    out[2] = (uint8_t)(0x84U | (q[2] & 0x01U)); /* QR, AA, keep RD */
    out[3] = 0x80U;                    /* RA, NOERROR */
    out[6] = 0;
    out[7] = answer ? 1U : 0U;         /* ANCOUNT */
    memset(&out[8], 0, 4);             /* NSCOUNT, ARCOUNT */
    if (answer) {
        static const uint8_t rr[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
        memcpy(&out[qend], rr, sizeof rr); /* name -> question, A, IN, TTL 60 */
        memcpy(&out[qend + 12U], ip, 4);
    }
    return total;
}
