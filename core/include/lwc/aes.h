/*
 * AES-128, one block at a time (FIPS-197). Software, OS-independent: the
 * gateway handles a few frames a minute, so speed does not matter and the core
 * stays testable on a PC. Table-based: not hardened against cache-timing
 * attacks, which need code on the same CPU.
 */
#ifndef LWC_AES_H
#define LWC_AES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t rk[176]; /* 11 round keys */
} lwc_aes128_t;

void lwc_aes128_init(lwc_aes128_t *ctx, const uint8_t key[16]);
void lwc_aes128_encrypt(const lwc_aes128_t *ctx, const uint8_t in[16], uint8_t out[16]);
void lwc_aes128_decrypt(const lwc_aes128_t *ctx, const uint8_t in[16], uint8_t out[16]);
/** Erase the round keys. */
void lwc_aes128_wipe(lwc_aes128_t *ctx);

/** AES-CMAC (RFC 4493) with a 128-bit key. */
void lwc_aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);

#ifdef __cplusplus
}
#endif

#endif /* LWC_AES_H */
