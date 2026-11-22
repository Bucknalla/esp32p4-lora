/*
 * Captive-portal DNS: answer every A query with one IPv4 address (the
 * setup access point), so a phone's connectivity probe reaches the gateway
 * and it opens its sign-in sheet (ADR-0013). The query is untrusted input.
 */
#ifndef LWC_DNS_H
#define LWC_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Build the reply to `q` in `out`. A/ANY questions get one A record with
 *  `ip`; other types get an empty NOERROR answer. Returns the reply length,
 *  or 0 when no reply should be sent (not a standard query, malformed,
 *  more than one question, or `cap` too small). */
size_t lwc_dns_captive_reply(const uint8_t *q, size_t len, const uint8_t ip[4], uint8_t *out,
                             size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LWC_DNS_H */
