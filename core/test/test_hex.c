#include "lwc/hex.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

int main(void)
{
    uint8_t b[4];
    CHECK(lwc_hex_decode("0aFf10e9", 8, b, 4) && b[0] == 0x0a && b[1] == 0xff && b[3] == 0xe9);
    CHECK(!lwc_hex_decode("0aFf10e", 7, b, 4));
    CHECK(!lwc_hex_decode("0aFf10e9aa", 10, b, 4));
    CHECK(!lwc_hex_decode("0aFg10e9", 8, b, 4));
    CHECK(!lwc_hex_decode("0a f10e9", 8, b, 4));
    uint64_t e;
    CHECK(lwc_hex_eui("70B3D57ED0001234", 16, &e) && e == 0x70B3D57ED0001234ULL);
    CHECK(!lwc_hex_eui("70B3D57ED000123", 15, &e));
    char s[9];
    const uint8_t in[4] = {0x00, 0xab, 0x10, 0xff};
    lwc_hex_encode(in, 4, s);
    CHECK(strcmp(s, "00AB10FF") == 0);
    if (failures != 0) {
        fprintf(stderr, "test_hex: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_hex: all passed\n");
    return 0;
}
