#include "lwc/logring.h"

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

static _Alignas(4) unsigned char mem[sizeof(lwc_logring_hdr_t) + 64];

static void w(lwc_logring_t *r, const char *s)
{
    lwc_logring_write(r, s, strlen(s));
}

static void test_fresh_and_retained(void)
{
    memset(mem, 0xA5, sizeof mem); /* garbage, as after power-on */
    lwc_logring_t r;
    CHECK(!lwc_logring_attach(&r, mem, sizeof mem));
    CHECK(r.size == 64);
    char out[128];
    CHECK(lwc_logring_tail(&r, out, sizeof out) == 0 && out[0] == '\0');

    w(&r, "one\ntwo\n");
    /* "reset": attach again to the same memory */
    lwc_logring_t r2;
    CHECK(lwc_logring_attach(&r2, mem, sizeof mem));
    CHECK(lwc_logring_tail(&r2, out, sizeof out) == 8 && strcmp(out, "one\ntwo\n") == 0);

    /* wrong size: not trusted */
    CHECK(!lwc_logring_attach(&r2, mem, sizeof mem - 4));
}

static void test_wrap_and_tail(void)
{
    lwc_logring_t r;
    (void)lwc_logring_attach(&r, mem, sizeof mem);
    lwc_logring_reset(&r);
    char line[32];
    for (int i = 0; i < 20; i++) {
        snprintf(line, sizeof line, "line %02d\n", i); /* 8 bytes */
        w(&r, line);
    }
    char out[128];
    /* 64 bytes kept = exactly the last 8 lines */
    CHECK(lwc_logring_tail(&r, out, sizeof out) == 64);
    CHECK(strncmp(out, "line 12\n", 8) == 0 && strcmp(out + 56, "line 19\n") == 0);
    /* smaller window cuts mid-line: starts at the next full line */
    CHECK(lwc_logring_tail(&r, out, 12) == 8 && strcmp(out, "line 19\n") == 0);
    /* a write larger than the ring keeps its last bytes */
    char big[100];
    memset(big, 'x', sizeof big);
    big[99] = '\n';
    lwc_logring_write(&r, big, sizeof big);
    CHECK(r.hdr->used == 64 && lwc_logring_tail(&r, out, sizeof out) == 64);
    CHECK(out[62] == 'x' && out[63] == '\n');
}

static void test_untrusted_header(void)
{
    lwc_logring_t r;
    (void)lwc_logring_attach(&r, mem, sizeof mem);
    lwc_logring_reset(&r);
    w(&r, "abc\n");
    lwc_logring_hdr_t *h = (lwc_logring_hdr_t *)mem;
    h->head = 1000; /* corrupt index, CRC now wrong */
    CHECK(!lwc_logring_attach(&r, mem, sizeof mem));
    CHECK(r.hdr->head == 0 && r.hdr->used == 0);

    /* non-printable bytes are masked */
    w(&r, "a\x01\x7f" "b\n");
    char out[16];
    CHECK(lwc_logring_tail(&r, out, sizeof out) == 5 && strcmp(out, "a??b\n") == 0);

    /* misuse */
    CHECK(!lwc_logring_attach(&r, mem, 10));
    CHECK(!lwc_logring_attach(&r, mem + 1, sizeof mem - 1));
    CHECK(!lwc_logring_attach(NULL, mem, sizeof mem));
    CHECK(lwc_logring_tail(NULL, out, sizeof out) == 0);
    CHECK(lwc_logring_tail(&r, out, 0) == 0);
}

int main(void)
{
    test_fresh_and_retained();
    test_wrap_and_tail();
    test_untrusted_header();
    if (failures != 0) {
        fprintf(stderr, "test_logring: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_logring: all passed\n");
    return 0;
}
