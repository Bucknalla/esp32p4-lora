#include "lwc/session.h"

#include <string.h>

static bool same(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (size_t i = 0; i < LWC_SESS_TOKEN_LEN; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0U;
}

static void wipe_slot(lwc_sess_slot_t *s)
{
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < sizeof *s; i++) {
        p[i] = 0;
    }
}

void lwc_sess_init(lwc_sess_table_t *t, uint32_t idle_s, uint32_t absolute_s)
{
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < LWC_SESS_MAX; i++) {
        wipe_slot(&t->slot[i]);
    }
    t->idle_s = idle_s;
    t->absolute_s = absolute_s;
}

static bool expired(const lwc_sess_table_t *t, const lwc_sess_slot_t *s, uint32_t now_s)
{
    return (uint32_t)(now_s - s->last_s) > t->idle_s ||
           (uint32_t)(now_s - s->created_s) > t->absolute_s;
}

void lwc_sess_create(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN],
                     lwc_role_t role, uint32_t now_s)
{
    if (t == NULL || token == NULL || role == LWC_ROLE_NONE) {
        return;
    }
    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < LWC_SESS_MAX; i++) {
        const lwc_sess_slot_t *s = &t->slot[i];
        if (s->role == LWC_ROLE_NONE || expired(t, s, now_s)) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) { /* least recently used */
        for (size_t i = 1; i < LWC_SESS_MAX; i++) {
            if ((uint32_t)(now_s - t->slot[i].last_s) > (uint32_t)(now_s - t->slot[pick].last_s)) {
                pick = i;
            }
        }
    }
    lwc_sess_slot_t *s = &t->slot[pick];
    memcpy(s->token, token, LWC_SESS_TOKEN_LEN);
    s->role = role;
    s->created_s = now_s;
    s->last_s = now_s;
}

lwc_role_t lwc_sess_check(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN],
                          uint32_t now_s)
{
    if (t == NULL || token == NULL) {
        return LWC_ROLE_NONE;
    }
    lwc_role_t role = LWC_ROLE_NONE;
    for (size_t i = 0; i < LWC_SESS_MAX; i++) {
        lwc_sess_slot_t *s = &t->slot[i];
        if (s->role == LWC_ROLE_NONE) {
            continue;
        }
        if (expired(t, s, now_s)) {
            wipe_slot(s);
            continue;
        }
        if (same(s->token, token) && role == LWC_ROLE_NONE) {
            s->last_s = now_s;
            role = s->role;
        }
    }
    return role;
}

void lwc_sess_end(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN])
{
    if (t == NULL || token == NULL) {
        return;
    }
    for (size_t i = 0; i < LWC_SESS_MAX; i++) {
        if (t->slot[i].role != LWC_ROLE_NONE && same(t->slot[i].token, token)) {
            wipe_slot(&t->slot[i]);
        }
    }
}

void lwc_sess_end_all(lwc_sess_table_t *t)
{
    if (t != NULL) {
        for (size_t i = 0; i < LWC_SESS_MAX; i++) {
            wipe_slot(&t->slot[i]);
        }
    }
}

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

bool lwc_sess_parse_cookie(const char *hdr, size_t len, uint8_t token[LWC_SESS_TOKEN_LEN])
{
    if (hdr == NULL || token == NULL) {
        return false;
    }
    static const char name[] = LWC_SESS_COOKIE "=";
    const size_t nl = sizeof name - 1U;
    size_t i = 0;
    while (i < len) {
        /* start of a cookie pair: skip separators and spaces */
        while (i < len && (hdr[i] == ' ' || hdr[i] == ';')) {
            i++;
        }
        size_t end = i;
        while (end < len && hdr[end] != ';') {
            end++;
        }
        if (end - i == nl + 2U * LWC_SESS_TOKEN_LEN && memcmp(&hdr[i], name, nl) == 0) {
            const char *v = &hdr[i + nl];
            bool ok = true;
            for (size_t k = 0; k < LWC_SESS_TOKEN_LEN; k++) {
                const int hi = hexval(v[2U * k]);
                const int lo = hexval(v[2U * k + 1U]);
                ok = ok && hi >= 0 && lo >= 0;
                token[k] = (uint8_t)(((unsigned)hi & 0xFU) << 4 | ((unsigned)lo & 0xFU));
            }
            if (ok) {
                return true;
            }
        }
        i = end + 1U;
    }
    memset(token, 0, LWC_SESS_TOKEN_LEN);
    return false;
}

void lwc_throttle_init(lwc_throttle_t *th, uint32_t max_fail, uint32_t lock_min_s,
                       uint32_t lock_max_s)
{
    if (th == NULL) {
        return;
    }
    memset(th, 0, sizeof *th);
    th->max_fail = max_fail;
    th->lock_min_s = lock_min_s;
    th->lock_max_s = lock_max_s;
    th->lock_s = lock_min_s;
}

uint32_t lwc_throttle_wait(const lwc_throttle_t *th, uint32_t now_s)
{
    if (th == NULL || th->locked_until_s == 0U) {
        return 0;
    }
    const int32_t left = (int32_t)(th->locked_until_s - now_s);
    return left > 0 ? (uint32_t)left : 0U;
}

void lwc_throttle_result(lwc_throttle_t *th, bool ok, uint32_t now_s)
{
    if (th == NULL) {
        return;
    }
    if (ok) {
        th->fails = 0;
        th->lock_s = th->lock_min_s;
        th->locked_until_s = 0;
        return;
    }
    if (++th->fails >= th->max_fail) {
        th->locked_until_s = now_s + th->lock_s;
        if (th->locked_until_s == 0U) {
            th->locked_until_s = 1U;
        }
        th->lock_s = (th->lock_s > th->lock_max_s / 2U) ? th->lock_max_s : th->lock_s * 2U;
        th->fails = 0;
    }
}
