#include "lwc/console_cmd.h"

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

static lwc_cmd_err_t parse(const char *s, lwc_cmd_t *c)
{
    return lwc_cmd_parse(s, strlen(s), c);
}

static void test_wifi_set(void)
{
    lwc_cmd_t c;
    /* "My Net" / "p@ss word,\"x" */
    CHECK(parse("WIFI_SET 4d79204e6574 7040737320776f72642c227822\r\n", &c) == LWC_CMD_OK);
    CHECK(c.kind == LWC_CMD_WIFI_SET);
    CHECK(strcmp(c.ssid, "My Net") == 0);
    CHECK(strcmp(c.pass, "p@ss word,\"x\"") == 0);

    lwc_cmd_wipe(&c);
    CHECK(c.pass[0] == '\0' && strcmp(c.ssid, "My Net") == 0);

    /* Upper-case hex and extra spaces between fields. */
    CHECK(parse("WIFI_SET  41   3132333435363738", &c) == LWC_CMD_OK);
    CHECK(strcmp(c.ssid, "A") == 0 && strcmp(c.pass, "12345678") == 0);
}

static void test_wifi_set_rejects(void)
{
    lwc_cmd_t c;
    CHECK(parse("WIFI_SET 41", &c) == LWC_CMD_ERR_SYNTAX);                      /* missing pass */
    CHECK(parse("WIFI_SET 41 3132333435363738 00", &c) == LWC_CMD_ERR_SYNTAX);  /* extra field */
    CHECK(parse("WIFI_SET 4 3132333435363738", &c) == LWC_CMD_ERR_SYNTAX);      /* odd hex */
    CHECK(parse("WIFI_SET 4g 3132333435363738", &c) == LWC_CMD_ERR_SYNTAX);     /* bad digit */
    CHECK(parse("WIFI_SET 0041 3132333435363738", &c) == LWC_CMD_ERR_SYNTAX);   /* NUL byte */
    CHECK(parse("WIFI_SET 41 31323334353637", &c) == LWC_CMD_ERR_LENGTH);       /* 7-char pass */
    CHECK(c.pass[0] == '\0'); /* secret wiped on error */

    char line[256];
    /* 33-byte SSID */
    strcpy(line, "WIFI_SET ");
    for (int i = 0; i < 33; i++) {
        strcat(line, "41");
    }
    strcat(line, " 3132333435363738");
    CHECK(parse(line, &c) == LWC_CMD_ERR_LENGTH);

    /* 64-byte password */
    strcpy(line, "WIFI_SET 41 ");
    for (int i = 0; i < 64; i++) {
        strcat(line, "31");
    }
    CHECK(parse(line, &c) == LWC_CMD_ERR_LENGTH);
}

static void test_other(void)
{
    lwc_cmd_t c;
    CHECK(parse("WIFI_CLEAR\n", &c) == LWC_CMD_OK && c.kind == LWC_CMD_WIFI_CLEAR);
    CHECK(parse("WIFI_CLEAR x", &c) == LWC_CMD_ERR_SYNTAX);
    CHECK(parse("TX_TEST\r\n", &c) == LWC_CMD_OK && c.kind == LWC_CMD_TX_TEST);
    CHECK(parse("TX_TEST 1", &c) == LWC_CMD_ERR_SYNTAX);
    CHECK(parse("", &c) == LWC_CMD_OK && c.kind == LWC_CMD_NONE);
    CHECK(parse("   \r\n", &c) == LWC_CMD_OK && c.kind == LWC_CMD_NONE);
    CHECK(parse("wifi_set 41 3132333435363738", &c) == LWC_CMD_ERR_UNKNOWN);
    CHECK(parse("WIFI_SET\t41 3132333435363738", &c) == LWC_CMD_ERR_SYNTAX);
    const char with_nul[] = "WIFI_CLEAR\0x";
    CHECK(lwc_cmd_parse(with_nul, sizeof with_nul - 1U, &c) == LWC_CMD_ERR_SYNTAX);
    CHECK(lwc_cmd_parse(NULL, 3, &c) == LWC_CMD_ERR_ARG);
    CHECK(lwc_cmd_parse("x", 1, NULL) == LWC_CMD_ERR_ARG);
}

int main(void)
{
    test_wifi_set();
    test_wifi_set_rejects();
    test_other();
    if (failures != 0) {
        fprintf(stderr, "test_console_cmd: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_console_cmd: all passed\n");
    return 0;
}
