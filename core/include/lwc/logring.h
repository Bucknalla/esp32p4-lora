/*
 * Log ring buffer kept in memory that survives a software reset or panic
 * (PSRAM .noinit on the gateway). Text is appended; the oldest bytes are
 * overwritten. After a reset the retained header is UNTRUSTED: attach checks
 * magic, size, indices and a CRC before any byte is read.
 */
#ifndef LWC_LOGRING_H
#define LWC_LOGRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_LOGRING_MAGIC 0x4C475231U /* "LGR1" */
#define LWC_LOGRING_MIN 64U           /* smallest data area */

typedef struct {
    uint32_t magic;
    uint32_t size; /* data bytes after the header */
    uint32_t head; /* next write offset, < size */
    uint32_t used; /* valid bytes, <= size */
    uint32_t crc;  /* CRC-32 of the four fields above */
} lwc_logring_hdr_t;

typedef struct {
    lwc_logring_hdr_t *hdr;
    uint8_t *data;
    uint32_t size;
} lwc_logring_t;

/** Bind to `mem` (4-byte aligned, `len` bytes incl. header). Returns true when
 *  the memory holds a valid ring of this size from before; false when it does
 *  not (the ring is then reset and empty). Returns false and leaves `r`
 *  unusable (size 0) when `mem` is too small. */
bool lwc_logring_attach(lwc_logring_t *r, void *mem, size_t len);

/** Empty the ring. */
void lwc_logring_reset(lwc_logring_t *r);

/** Append `n` bytes (only the last `size` bytes are kept). */
void lwc_logring_write(lwc_logring_t *r, const char *s, size_t n);

/** Copy the newest text, at most `cap - 1` bytes, NUL-terminated. When the
 *  copy starts mid-line, it starts after the first newline. Bytes other than
 *  printable ASCII, tab and newline become '?'. Returns the length. */
size_t lwc_logring_tail(const lwc_logring_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LWC_LOGRING_H */
