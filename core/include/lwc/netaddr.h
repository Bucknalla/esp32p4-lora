/*
 * LAN address test for the management web server (ADR-0010): private,
 * link-local and loopback IPv4; IPv6 unique-local, link-local and loopback;
 * IPv4-mapped IPv6 by its IPv4 part. Everything else is not LAN.
 */
#ifndef LWC_NETADDR_H
#define LWC_NETADDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `addr` in network byte order, `len` 4 (IPv4) or 16 (IPv6). */
bool lwc_ip_is_lan(const uint8_t *addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LWC_NETADDR_H */
