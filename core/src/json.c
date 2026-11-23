#include "lwc/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==== reader ==============================================================
 * Enough for txpk and lbs: objects, strings without escapes in values we use (escapes
 * are skipped correctly elsewhere), numbers, true/false/null, and arrays or
 * nested objects in keys we skip. Depth-limited; never reads past `end`. */

void lwc_jr_ws(lwc_jr_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

bool lwc_jr_lit(lwc_jr_t *r, char c)
{
    lwc_jr_ws(r);
    if (r->p < r->end && *r->p == c) {
        r->p++;
        return 1;
    }
    return 0;
}

/* String: on success sets [*s, *n) to the raw contents (escapes not decoded). */
bool lwc_jr_str(lwc_jr_t *r, const char **s, size_t *n)
{
    lwc_jr_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        return 0;
    }
    r->p++;
    const char *start = r->p;
    while (r->p < r->end && *r->p != '"') {
        if ((unsigned char)*r->p < 0x20U) {
            return 0;
        }
        if (*r->p == '\\') {
            r->p++;
            if (r->p >= r->end) {
                return 0;
            }
        }
        r->p++;
    }
    if (r->p >= r->end) {
        return 0;
    }
    *s = start;
    *n = (size_t)(r->p - start);
    r->p++;
    return 1;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Number token: sets [*s, *n). Validates JSON number syntax. */
bool lwc_jr_num(lwc_jr_t *r, const char **s, size_t *n)
{
    lwc_jr_ws(r);
    const char *start = r->p;
    if (r->p < r->end && *r->p == '-') {
        r->p++;
    }
    if (r->p >= r->end || !is_digit(*r->p)) {
        return 0;
    }
    while (r->p < r->end && is_digit(*r->p)) {
        r->p++;
    }
    if (r->p < r->end && *r->p == '.') {
        r->p++;
        if (r->p >= r->end || !is_digit(*r->p)) {
            return 0;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) {
            r->p++;
        }
        if (r->p >= r->end || !is_digit(*r->p)) {
            return 0;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    *s = start;
    *n = (size_t)(r->p - start);
    return 1;
}

bool lwc_jr_word(lwc_jr_t *r, const char *w)
{
    lwc_jr_ws(r);
    const size_t n = strlen(w);
    if ((size_t)(r->end - r->p) >= n && memcmp(r->p, w, n) == 0) {
        r->p += n;
        return 1;
    }
    return 0;
}

static int skip_value(lwc_jr_t *r, int depth);

static int skip_container(lwc_jr_t *r, int depth, char close, int is_obj)
{
    if (depth > JSON_MAX_DEPTH) {
        return 0;
    }
    if (lwc_jr_lit(r, close)) {
        return 1;
    }
    for (;;) {
        if (is_obj) {
            const char *k;
            size_t kn;
            if (!lwc_jr_str(r, &k, &kn) || !lwc_jr_lit(r, ':')) {
                return 0;
            }
        }
        if (!skip_value(r, depth + 1)) {
            return 0;
        }
        if (lwc_jr_lit(r, ',')) {
            continue;
        }
        return lwc_jr_lit(r, close);
    }
}

static int skip_value(lwc_jr_t *r, int depth)
{
    const char *s;
    size_t n;
    lwc_jr_ws(r);
    if (r->p >= r->end) {
        return 0;
    }
    switch (*r->p) {
    case '{': r->p++; return skip_container(r, depth, '}', 1);
    case '[': r->p++; return skip_container(r, depth, ']', 0);
    case '"': return lwc_jr_str(r, &s, &n);
    case 't': return lwc_jr_word(r, "true");
    case 'f': return lwc_jr_word(r, "false");
    case 'n': return lwc_jr_word(r, "null");
    default: return lwc_jr_num(r, &s, &n);
    }
}

bool lwc_jr_key_is(const char *k, size_t kn, const char *w)
{
    return kn == strlen(w) && memcmp(k, w, kn) == 0;
}

bool lwc_jr_skip(lwc_jr_t *r)
{
    return skip_value(r, 0) != 0;
}

bool lwc_jr_i64(const char *s, size_t n, int64_t *out)
{
    if (s == NULL || out == NULL || n == 0U || n > 20U) {
        return false;
    }
    size_t i = 0;
    const bool neg = s[0] == '-';
    if (neg) {
        i = 1;
    }
    if (i >= n) {
        return false;
    }
    uint64_t v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false; /* fraction or exponent: not an integer */
        }
        const uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10U) {
            return false;
        }
        v = v * 10U + d;
    }
    if (neg ? v > (uint64_t)INT64_MAX + 1U : v > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = neg ? (int64_t)(0U - v) : (int64_t)v;
    return true;
}

void lwc_sb_printf(lwc_sb_t *b, const char *fmt, ...)
{
    if (b->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    b->len += (size_t)n;
}
