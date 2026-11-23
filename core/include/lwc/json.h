/*
 * Minimal JSON reader and writer for the Basics Station (lbs) code. The reader walks a buffer that is never read past
 * `end`; strings are returned raw (escapes not decoded). No allocation.
 */
#ifndef LWC_JSON_H
#define LWC_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_MAX_DEPTH 8

typedef struct {
    const char *p;
    const char *end;
} lwc_jr_t;

void lwc_jr_ws(lwc_jr_t *r);
/** Consume `c` (after white space) if it is next. */
bool lwc_jr_lit(lwc_jr_t *r, char c);
/** String: sets [*s, *n) to the raw contents. */
bool lwc_jr_str(lwc_jr_t *r, const char **s, size_t *n);
/** Number token: sets [*s, *n); checks JSON number syntax. */
bool lwc_jr_num(lwc_jr_t *r, const char **s, size_t *n);
bool lwc_jr_word(lwc_jr_t *r, const char *w);
/** Skip one value of any type (depth-limited). */
bool lwc_jr_skip(lwc_jr_t *r);
bool lwc_jr_key_is(const char *k, size_t kn, const char *w);
/** Exact 64-bit integer from a number token: false for a fraction, an exponent or overflow. */
bool lwc_jr_i64(const char *s, size_t n, int64_t *out);

/** Output buffer with sticky overflow. */
typedef struct {
    char *p;
    size_t cap;
    size_t len;
    int overflow;
} lwc_sb_t;

void lwc_sb_printf(lwc_sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* LWC_JSON_H */
