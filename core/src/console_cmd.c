#include "lwc/console_cmd.h"

#include <string.h>

#define MAX_FIELDS 3U

typedef struct {
    const char *p;
    size_t len;
} field_t;

static int hex_nibble(char c)
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

/* Decode hex into out (cap bytes, plus a NUL). Rejects odd length, bad
 * digits, embedded NUL bytes and values outside [min, cap]. */
static lwc_cmd_err_t hex_to_str(const field_t *f, char *out, size_t min, size_t cap)
{
    if ((f->len % 2U) != 0U) {
        return LWC_CMD_ERR_SYNTAX;
    }
    const size_t n = f->len / 2U;
    if (n < min || n > cap) {
        return LWC_CMD_ERR_LENGTH;
    }
    for (size_t i = 0; i < n; i++) {
        const int hi = hex_nibble(f->p[2U * i]);
        const int lo = hex_nibble(f->p[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            return LWC_CMD_ERR_SYNTAX;
        }
        const char c = (char)((hi << 4) | lo);
        if (c == '\0') {
            return LWC_CMD_ERR_SYNTAX;
        }
        out[i] = c;
    }
    out[n] = '\0';
    return LWC_CMD_OK;
}

static size_t split(const char *line, size_t len, field_t *f, size_t max)
{
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && line[i] == ' ') {
            i++;
        }
        if (i >= len) {
            break;
        }
        const size_t start = i;
        while (i < len && line[i] != ' ') {
            i++;
        }
        if (n == max) {
            return max + 1U; /* too many fields */
        }
        f[n].p = &line[start];
        f[n].len = i - start;
        n++;
    }
    return n;
}

static int word_is(const field_t *f, const char *w)
{
    const size_t wl = strlen(w);
    return f->len == wl && memcmp(f->p, w, wl) == 0;
}

lwc_cmd_err_t lwc_cmd_parse(const char *line, size_t len, lwc_cmd_t *out)
{
    if (line == NULL || out == NULL) {
        return LWC_CMD_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));

    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        len--;
    }
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\0' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n') {
            return LWC_CMD_ERR_SYNTAX;
        }
    }

    field_t f[MAX_FIELDS];
    const size_t n = split(line, len, f, MAX_FIELDS);
    if (n == 0U) {
        out->kind = LWC_CMD_NONE;
        return LWC_CMD_OK;
    }
    if (n > MAX_FIELDS) {
        return LWC_CMD_ERR_SYNTAX;
    }

    if (word_is(&f[0], "WIFI_SET")) {
        if (n != 3U) {
            return LWC_CMD_ERR_SYNTAX;
        }
        lwc_cmd_err_t e = hex_to_str(&f[1], out->ssid, 1U, LWC_WIFI_SSID_MAX);
        if (e == LWC_CMD_OK) {
            e = hex_to_str(&f[2], out->pass, LWC_WIFI_PASS_MIN, LWC_WIFI_PASS_MAX);
        }
        if (e != LWC_CMD_OK) {
            lwc_cmd_wipe(out);
            return e;
        }
        out->kind = LWC_CMD_WIFI_SET;
        return LWC_CMD_OK;
    }
    if (word_is(&f[0], "WIFI_CLEAR")) {
        if (n != 1U) {
            return LWC_CMD_ERR_SYNTAX;
        }
        out->kind = LWC_CMD_WIFI_CLEAR;
        return LWC_CMD_OK;
    }
    if (word_is(&f[0], "TX_TEST")) {
        if (n != 1U) {
            return LWC_CMD_ERR_SYNTAX;
        }
        out->kind = LWC_CMD_TX_TEST;
        return LWC_CMD_OK;
    }
    if (word_is(&f[0], "OTA_CHECK")) {
        if (n != 1U) {
            return LWC_CMD_ERR_SYNTAX;
        }
        out->kind = LWC_CMD_OTA_CHECK;
        return LWC_CMD_OK;
    }
    static const struct {
        const char *word;
        lwc_cmd_kind_t kind;
    } bare[] = {
        {"STATUS", LWC_CMD_STATUS},
        {"TEST_ABORT", LWC_CMD_TEST_ABORT},
        {"TEST_STALL", LWC_CMD_TEST_STALL},
    };
    for (size_t i = 0; i < sizeof bare / sizeof bare[0]; i++) {
        if (word_is(&f[0], bare[i].word)) {
            if (n != 1U) {
                return LWC_CMD_ERR_SYNTAX;
            }
            out->kind = bare[i].kind;
            return LWC_CMD_OK;
        }
    }
    return LWC_CMD_ERR_UNKNOWN;
}

void lwc_cmd_wipe(lwc_cmd_t *cmd)
{
    if (cmd != NULL) {
        volatile char *p = (volatile char *)cmd->pass;
        for (size_t i = 0; i < sizeof(cmd->pass); i++) {
            p[i] = '\0';
        }
    }
}
