#include "lwc/session.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void tok(uint8_t t[LWC_SESS_TOKEN_LEN], uint8_t v)
{
    memset(t, v, LWC_SESS_TOKEN_LEN);
}

static void test_sessions(void)
{
    lwc_sess_table_t t;
    lwc_sess_init(&t, 1800, 43200);
    uint8_t a[16], b[16], x[16];
    tok(a, 0xA1);
    tok(b, 0xB2);
    tok(x, 0xEE);
    lwc_sess_create(&t, a, LWC_ROLE_ADMIN, 100);
    lwc_sess_create(&t, b, LWC_ROLE_VIEWER, 100);
    CHECK(lwc_sess_check(&t, a, 200) == LWC_ROLE_ADMIN);
    CHECK(lwc_sess_check(&t, b, 200) == LWC_ROLE_VIEWER);
    CHECK(lwc_sess_check(&t, x, 200) == LWC_ROLE_NONE);
    /* idle: a used at 200, so alive at 1999, b too; at 2001 both idle-expired */
    CHECK(lwc_sess_check(&t, a, 1999) == LWC_ROLE_ADMIN);
    CHECK(lwc_sess_check(&t, b, 2001) == LWC_ROLE_NONE);
    /* absolute: keep a busy, it still ends 12 h after login */
    uint32_t now = 1999;
    while (now + 1000U <= 43000U) {
        now += 1000;
        CHECK(lwc_sess_check(&t, a, now) == LWC_ROLE_ADMIN);
    }
    CHECK(lwc_sess_check(&t, a, 43300) == LWC_ROLE_ADMIN); /* 43200 s after login */
    CHECK(lwc_sess_check(&t, a, 43301) == LWC_ROLE_NONE);
    /* logout and end_all */
    lwc_sess_create(&t, a, LWC_ROLE_ADMIN, 50000);
    lwc_sess_end(&t, a);
    CHECK(lwc_sess_check(&t, a, 50001) == LWC_ROLE_NONE);
    lwc_sess_create(&t, b, LWC_ROLE_VIEWER, 50000);
    lwc_sess_end_all(&t);
    CHECK(lwc_sess_check(&t, b, 50001) == LWC_ROLE_NONE);
    /* full table: least recently used goes */
    for (uint8_t i = 0; i < LWC_SESS_MAX; i++) {
        uint8_t k[16];
        tok(k, (uint8_t)(i + 1));
        lwc_sess_create(&t, k, LWC_ROLE_VIEWER, 60000U + i);
    }
    uint8_t k1[16];
    tok(k1, 1);
    CHECK(lwc_sess_check(&t, k1, 60010) == LWC_ROLE_VIEWER); /* k1 now newest */
    lwc_sess_create(&t, x, LWC_ROLE_ADMIN, 60011);           /* evicts k2 */
    uint8_t k2[16];
    tok(k2, 2);
    CHECK(lwc_sess_check(&t, k2, 60012) == LWC_ROLE_NONE);
    CHECK(lwc_sess_check(&t, k1, 60012) == LWC_ROLE_VIEWER);
    CHECK(lwc_sess_check(&t, x, 60012) == LWC_ROLE_ADMIN);
    lwc_sess_create(&t, x, LWC_ROLE_NONE, 1); /* ignored */
}

static void test_cookie(void)
{
    uint8_t t[16];
    const char *ok = "theme=dark; gw_session=00112233445566778899aabbccddeeff; x=1";
    CHECK(lwc_sess_parse_cookie(ok, strlen(ok), t) && t[0] == 0x00 && t[15] == 0xff);
    const char *only = "gw_session=FFEEDDCCBBAA99887766554433221100";
    CHECK(lwc_sess_parse_cookie(only, strlen(only), t) && t[0] == 0xff);
    const char *bad[] = {
        "gw_session=00112233445566778899aabbccddeef",   /* 31 digits */
        "gw_session=00112233445566778899aabbccddeeff0", /* 33 */
        "gw_session=0011223344556677889zaabbccddeeff",  /* bad digit */
        "xgw_session=00112233445566778899aabbccddeeff", /* other name */
        "",
        ";;;;",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!lwc_sess_parse_cookie(bad[i], strlen(bad[i]), t));
    }
    CHECK(!lwc_sess_parse_cookie(NULL, 3, t));
    /* length limits the scan: the cookie is cut */
    CHECK(!lwc_sess_parse_cookie(only, 20, t));
}

static void test_throttle(void)
{
    lwc_throttle_t th;
    lwc_throttle_init(&th, 5, 60, 900);
    for (int i = 0; i < 4; i++) {
        lwc_throttle_result(&th, false, 10);
    }
    CHECK(lwc_throttle_wait(&th, 10) == 0);
    lwc_throttle_result(&th, false, 10);
    CHECK(lwc_throttle_wait(&th, 10) == 60 && lwc_throttle_wait(&th, 69) == 1);
    CHECK(lwc_throttle_wait(&th, 70) == 0);
    for (int i = 0; i < 5; i++) {
        lwc_throttle_result(&th, false, 70);
    }
    CHECK(lwc_throttle_wait(&th, 70) == 120); /* doubled */
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 5; i++) {
            lwc_throttle_result(&th, false, 1000);
        }
    }
    CHECK(lwc_throttle_wait(&th, 1000) == 900); /* capped */
    lwc_throttle_result(&th, true, 2000);
    CHECK(lwc_throttle_wait(&th, 2000) == 0 && th.lock_s == 60);
}

int main(void)
{
    test_sessions();
    test_cookie();
    test_throttle();
    if (failures != 0) {
        fprintf(stderr, "test_session: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_session: all passed\n");
    return 0;
}
