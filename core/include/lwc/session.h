/*
 * Web login sessions and login throttling (management UI/API, ADR-0010).
 * Pure logic: the caller supplies random tokens and the time. Tokens are
 * 16 random bytes, sent as 32 hex digits in the "gw_session" cookie.
 * Cookie headers are untrusted input.
 */
#ifndef LWC_SESSION_H
#define LWC_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_SESS_TOKEN_LEN 16U
#define LWC_SESS_MAX 8U
#define LWC_SESS_COOKIE "gw_session"

typedef enum {
    LWC_ROLE_NONE = 0,
    LWC_ROLE_VIEWER = 1,
    LWC_ROLE_ADMIN = 2,
} lwc_role_t;

typedef struct {
    uint8_t token[LWC_SESS_TOKEN_LEN];
    lwc_role_t role; /* NONE: free slot */
    uint32_t created_s;
    uint32_t last_s;
} lwc_sess_slot_t;

typedef struct {
    lwc_sess_slot_t slot[LWC_SESS_MAX];
    uint32_t idle_s;     /* a session ends after this long unused */
    uint32_t absolute_s; /* and this long after login, used or not */
} lwc_sess_table_t;

void lwc_sess_init(lwc_sess_table_t *t, uint32_t idle_s, uint32_t absolute_s);

/** Store a new session; when full, the least recently used one is dropped. */
void lwc_sess_create(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN],
                     lwc_role_t role, uint32_t now_s);

/** Role for a token (constant-time compare per slot), NONE when unknown or
 *  expired; refreshes the idle timer. */
lwc_role_t lwc_sess_check(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN],
                          uint32_t now_s);

/** End one session (logout). */
void lwc_sess_end(lwc_sess_table_t *t, const uint8_t token[LWC_SESS_TOKEN_LEN]);

/** End every session (password change). */
void lwc_sess_end_all(lwc_sess_table_t *t);

/** Find "gw_session=<32 hex>" in a Cookie header value. */
bool lwc_sess_parse_cookie(const char *hdr, size_t len, uint8_t token[LWC_SESS_TOKEN_LEN]);

/* ---- login throttling ------------------------------------------------------
 * After `max_fail` failures in a row, logins are refused for `lock_s`
 * seconds (doubling each further lockout, up to `lock_max_s`). One counter
 * for the whole device: the UI is LAN-only and has few users. */
typedef struct {
    uint32_t fails;
    uint32_t locked_until_s;
    uint32_t lock_s;
    uint32_t max_fail;
    uint32_t lock_min_s;
    uint32_t lock_max_s;
} lwc_throttle_t;

void lwc_throttle_init(lwc_throttle_t *th, uint32_t max_fail, uint32_t lock_min_s,
                       uint32_t lock_max_s);

/** Seconds until a login may be tried (0: now). */
uint32_t lwc_throttle_wait(const lwc_throttle_t *th, uint32_t now_s);

void lwc_throttle_result(lwc_throttle_t *th, bool ok, uint32_t now_s);

#ifdef __cplusplus
}
#endif

#endif /* LWC_SESSION_H */
