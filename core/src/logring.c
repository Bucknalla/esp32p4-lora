#include "lwc/logring.h"

#include <string.h>

static uint32_t crc32_words(const uint32_t *w, size_t n)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++) {
        for (unsigned b = 0; b < 4U; b++) {
            crc ^= (w[i] >> (8U * b)) & 0xFFU;
            for (unsigned k = 0; k < 8U; k++) {
                crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
        }
    }
    return ~crc;
}

static uint32_t hdr_crc(const lwc_logring_hdr_t *h)
{
    const uint32_t f[4] = {h->magic, h->size, h->head, h->used};
    return crc32_words(f, 4);
}

static void seal(lwc_logring_hdr_t *h)
{
    h->crc = hdr_crc(h);
}

bool lwc_logring_attach(lwc_logring_t *r, void *mem, size_t len)
{
    if (r == NULL) {
        return false;
    }
    r->hdr = NULL;
    r->data = NULL;
    r->size = 0;
    if (mem == NULL || ((uintptr_t)mem % 4U) != 0U || len < sizeof(lwc_logring_hdr_t) + LWC_LOGRING_MIN ||
        len - sizeof(lwc_logring_hdr_t) > UINT32_MAX) {
        return false;
    }
    r->hdr = (lwc_logring_hdr_t *)mem;
    r->data = (uint8_t *)mem + sizeof(lwc_logring_hdr_t);
    r->size = (uint32_t)(len - sizeof(lwc_logring_hdr_t));

    const lwc_logring_hdr_t h = *r->hdr; /* one snapshot: memory may be garbage */
    const bool valid = h.magic == LWC_LOGRING_MAGIC && h.size == r->size && h.head < h.size &&
                       h.used <= h.size && h.crc == hdr_crc(&h);
    if (!valid) {
        lwc_logring_reset(r);
    }
    return valid;
}

void lwc_logring_reset(lwc_logring_t *r)
{
    if (r == NULL || r->hdr == NULL) {
        return;
    }
    r->hdr->magic = LWC_LOGRING_MAGIC;
    r->hdr->size = r->size;
    r->hdr->head = 0;
    r->hdr->used = 0;
    seal(r->hdr);
}

void lwc_logring_write(lwc_logring_t *r, const char *s, size_t n)
{
    if (r == NULL || r->hdr == NULL || s == NULL || n == 0U) {
        return;
    }
    if (n > r->size) {
        s += n - r->size;
        n = r->size;
    }
    uint32_t head = r->hdr->head;
    const uint32_t len = (uint32_t)n;
    const uint32_t first = (len < r->size - head) ? len : r->size - head;
    memcpy(&r->data[head], s, first);
    memcpy(&r->data[0], s + first, len - first);
    head += len;
    if (head >= r->size) {
        head -= r->size;
    }
    const uint32_t used = r->hdr->used;
    r->hdr->head = head;
    r->hdr->used = (r->size - used < len) ? r->size : used + len;
    seal(r->hdr);
}

size_t lwc_logring_tail(const lwc_logring_t *r, char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return 0;
    }
    out[0] = '\0';
    if (r == NULL || r->hdr == NULL || r->size == 0U) {
        return 0;
    }
    const uint32_t used = r->hdr->used <= r->size ? r->hdr->used : r->size;
    const uint32_t head = r->hdr->head < r->size ? r->hdr->head : 0U;
    uint32_t n = used;
    if ((size_t)n > cap - 1U) {
        n = (uint32_t)(cap - 1U);
    }
    /* Oldest byte to copy. */
    uint32_t pos = (head + r->size - n) % r->size;
    size_t o = 0;
    bool skip = n < used; /* cut mid-history: drop the partial first line */
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = r->data[pos];
        pos = (pos + 1U == r->size) ? 0U : pos + 1U;
        if (skip) {
            skip = c != '\n';
            continue;
        }
        out[o++] = (c == '\n' || c == '\t' || (c >= 0x20U && c < 0x7FU)) ? (char)c : '?';
    }
    out[o] = '\0';
    return o;
}
