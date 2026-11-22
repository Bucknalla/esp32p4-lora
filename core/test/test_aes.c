/* AES-128 (FIPS-197 appendix C.1) and AES-CMAC (RFC 4493 section 4). */
#include "lwc/aes.h"

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

static void hex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i] != '\0'; i++) {
        unsigned v;
        sscanf(&s[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

int main(void)
{
    uint8_t key[16], pt[16], ct[16], out[16];
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("00112233445566778899aabbccddeeff", pt);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
    lwc_aes128_t ctx;
    lwc_aes128_init(&ctx, key);
    lwc_aes128_encrypt(&ctx, pt, out);
    CHECK(memcmp(out, ct, 16) == 0);
    lwc_aes128_decrypt(&ctx, ct, out);
    CHECK(memcmp(out, pt, 16) == 0);

    uint8_t k[16], msg[64], mac[16], want[16];
    hex("2b7e151628aed2a6abf7158809cf4f3c", k);
    hex("6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710", msg);
    static const struct {
        size_t len;
        const char *mac;
    } v[] = {
        {0, "bb1d6929e95937287fa37d129b756746"},
        {16, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40, "dfa66747de9ae63030ca32611497c827"},
        {64, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        hex(v[i].mac, want);
        lwc_aes_cmac(k, v[i].len ? msg : NULL, v[i].len, mac);
        CHECK(memcmp(mac, want, 16) == 0);
    }
    if (failures != 0) {
        fprintf(stderr, "test_aes: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_aes: all passed\n");
    return 0;
}
