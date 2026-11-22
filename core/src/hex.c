#include "lwc/hex.h"

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool lwc_hex_decode(const char *s, size_t len, uint8_t *out, size_t n)
{
    if (s == NULL || out == NULL || len != 2U * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const int hi = nibble(s[2U * i]), lo = nibble(s[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool lwc_hex_eui(const char *s, size_t len, uint64_t *out)
{
    uint8_t b[8];
    if (out == NULL || !lwc_hex_decode(s, len, b, sizeof b)) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof b; i++) {
        v = (v << 8) | b[i];
    }
    *out = v;
    return true;
}

void lwc_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2U * i] = d[in[i] >> 4];
        out[2U * i + 1U] = d[in[i] & 0x0FU];
    }
    out[2U * n] = '\0';
}
