#include "lwc/supervisor.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void)
{
    lwc_sv_t sv;
    lwc_sv_init(&sv);
    uint32_t late = 0;
    const int a = lwc_sv_register(&sv, "radio", 30000, 1000);
    const int b = lwc_sv_register(&sv, "ota", 180000, 1000);
    CHECK(a == 0 && b == 1);
    CHECK(lwc_sv_register(&sv, NULL, 1, 0) == -1 && lwc_sv_register(&sv, "x", 0, 0) == -1);

    CHECK(lwc_sv_expired(&sv, 31000, NULL) == -1);         /* exactly at budget: fine */
    CHECK(lwc_sv_expired(&sv, 31001, &late) == a && late == 30001);
    lwc_sv_beat(&sv, a, 31001);
    CHECK(lwc_sv_expired(&sv, 61000, NULL) == -1);
    CHECK(lwc_sv_expired(&sv, 181002, NULL) == a);          /* radio first */
    lwc_sv_beat(&sv, a, 181002);
    CHECK(lwc_sv_expired(&sv, 181002, NULL) == b);

    /* pause, then a beat resumes */
    lwc_sv_pause(&sv, b);
    CHECK(lwc_sv_expired(&sv, 200000, NULL) == -1);
    lwc_sv_beat(&sv, b, 200000);
    CHECK(lwc_sv_expired(&sv, 380001, NULL) == a);

    /* counter wrap */
    lwc_sv_t w;
    lwc_sv_init(&w);
    const int c = lwc_sv_register(&w, "c", 1000, 0xFFFFFF00U);
    CHECK(lwc_sv_expired(&w, 0x00000100U, NULL) == -1); /* 512 ms later */
    CHECK(lwc_sv_expired(&w, 0x00000400U, NULL) == c);  /* 1280 ms later */

    /* full */
    lwc_sv_t f;
    lwc_sv_init(&f);
    for (int i = 0; i < LWC_SV_MAX; i++) {
        CHECK(lwc_sv_register(&f, "t", 1, 0) == i);
    }
    CHECK(lwc_sv_register(&f, "t", 1, 0) == -1);
    lwc_sv_beat(&f, 99, 0);  /* ignored */
    lwc_sv_pause(&f, -1);    /* ignored */

    if (failures != 0) {
        fprintf(stderr, "test_supervisor: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_supervisor: all passed\n");
    return 0;
}
