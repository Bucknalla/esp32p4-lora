#include "lwc/form.h"

#include <string.h>

static int hexval(char c)
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

int lwc_form_get(const char *body, size_t len, const char *key, char *out, size_t cap)
{
    if (body == NULL || key == NULL || out == NULL || cap == 0U) {
        return -1;
    }
    out[0] = '\0';
    const size_t kl = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && body[end] != '&') {
            end++;
        }
        if (end - i > kl && memcmp(&body[i], key, kl) == 0 && body[i + kl] == '=') {
            size_t o = 0;
            for (size_t p = i + kl + 1U; p < end; p++) {
                char c = body[p];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (end - p < 3U) { /* fewer than two digits left */
                        out[0] = '\0';
                        return -1;
                    }
                    const int hi = hexval(body[p + 1U]);
                    const int lo = hexval(body[p + 2U]);
                    if (hi < 0 || lo < 0) {
                        out[0] = '\0';
                        return -1;
                    }
                    c = (char)((hi << 4) | lo);
                    p += 2U;
                }
                if (c == '\0' || o + 1U >= cap) {
                    out[0] = '\0';
                    return -1;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return (int)o;
        }
        i = end + 1U;
    }
    return -1;
}
