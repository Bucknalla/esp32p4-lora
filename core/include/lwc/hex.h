/* Hex text <-> bytes for EUIs and keys typed into the web UI (untrusted). */
#ifndef LWC_HEX_H
#define LWC_HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decode exactly `n` bytes from `len` characters of hex (upper or lower case,
 *  no separators). False on any other length or a non-hex character. */
bool lwc_hex_decode(const char *s, size_t len, uint8_t *out, size_t n);

/** 16 hex digits -> EUI-64 value (big-endian text, as printed). */
bool lwc_hex_eui(const char *s, size_t len, uint64_t *out);

/** Upper-case hex of `n` bytes into `out` (2n+1 bytes, NUL-terminated). */
void lwc_hex_encode(const uint8_t *in, size_t n, char *out);

#ifdef __cplusplus
}
#endif

#endif /* LWC_HEX_H */
