/*
 * application/x-www-form-urlencoded field lookup (web login, ADR-0010).
 * The body is untrusted: every length is checked and nothing is allocated.
 */
#ifndef LWC_FORM_H
#define LWC_FORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Find `key` in `body` and percent-decode its value ('+' = space) into
 *  `out` (NUL-terminated). Returns the value length, or -1 when the key is
 *  missing, the value does not fit in `cap - 1` bytes, holds a bad escape
 *  or decodes to a NUL byte. The first occurrence wins. */
int lwc_form_get(const char *body, size_t len, const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LWC_FORM_H */
