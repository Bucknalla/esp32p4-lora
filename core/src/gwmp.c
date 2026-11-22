#include "lwc/gwmp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- small output buffer with sticky overflow ---------------------------- */

typedef struct {
    char *p;
    size_t cap;
    size_t len;
    int overflow;
} sbuf_t;

static void sb_printf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    if (b->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    b->len += (size_t)n;
}

/* ---- time ---------------------------------------------------------------- */

/* Days since 1970-01-01 to civil date (H. Hinnant's algorithm). */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    *d = doy - (153U * mp + 2U) / 5U + 1U;
    *m = mp < 10U ? mp + 3U : mp - 9U;
    *y = (int64_t)yoe + era * 400 + (*m <= 2U ? 1 : 0);
}

typedef struct {
    int64_t year;
    unsigned mon, day, hh, mm, ss;
} civil_t;

static int to_civil(int64_t unix_s, civil_t *c)
{
    if (unix_s < 0 || unix_s > 253402300799LL) { /* 9999-12-31T23:59:59 */
        return -1;
    }
    const int64_t days = unix_s / 86400;
    const int64_t secs = unix_s % 86400;
    civil_from_days(days, &c->year, &c->mon, &c->day);
    c->hh = (unsigned)(secs / 3600);
    c->mm = (unsigned)((secs % 3600) / 60);
    c->ss = (unsigned)(secs % 60);
    return 0;
}

int lwc_time_iso8601_us(int64_t unix_us, char *out, size_t cap)
{
    civil_t c;
    if (out == NULL || cap < 28U || unix_us < 0 || to_civil(unix_us / 1000000, &c) != 0) {
        return -1;
    }
    const int n = snprintf(out, cap, "%04lld-%02u-%02uT%02u:%02u:%02u.%06luZ", (long long)c.year,
                           c.mon, c.day, c.hh, c.mm, c.ss,
                           (unsigned long)(unix_us % 1000000));
    return (n == 27) ? 0 : -1;
}

int lwc_time_stat(int64_t unix_s, char *out, size_t cap)
{
    civil_t c;
    if (out == NULL || cap < 24U || to_civil(unix_s, &c) != 0) {
        return -1;
    }
    const int n = snprintf(out, cap, "%04lld-%02u-%02u %02u:%02u:%02u GMT", (long long)c.year,
                           c.mon, c.day, c.hh, c.mm, c.ss);
    return (n == 23) ? 0 : -1;
}

/* ---- base64 -------------------------------------------------------------- */

size_t lwc_base64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = 4U * ((n + 2U) / 3U);
    if (out == NULL || cap < need + 1U || (in == NULL && n > 0U)) {
        if (out != NULL && cap > 0U) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3U) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = (i + 1U < n) ? in[i + 1U] : 0U;
        const uint32_t b2 = (i + 2U < n) ? in[i + 2U] : 0U;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = tbl[(v >> 18) & 0x3FU];
        out[o++] = tbl[(v >> 12) & 0x3FU];
        out[o++] = (i + 1U < n) ? tbl[(v >> 6) & 0x3FU] : '=';
        out[o++] = (i + 2U < n) ? tbl[v & 0x3FU] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ---- framing ------------------------------------------------------------- */

size_t lwc_gwmp_header(uint8_t *buf, size_t cap, lwc_gwmp_type_t type, uint16_t token,
                       const uint8_t eui[8])
{
    const size_t need = (eui != NULL) ? LWC_GWMP_HDR_UP : LWC_GWMP_HDR_DOWN;
    if (buf == NULL || cap < need) {
        return 0;
    }
    buf[0] = LWC_GWMP_VERSION;
    buf[1] = (uint8_t)(token >> 8);
    buf[2] = (uint8_t)(token & 0xFFU);
    buf[3] = (uint8_t)type;
    if (eui != NULL) {
        memcpy(&buf[4], eui, 8);
    }
    return need;
}

void lwc_eui64_from_mac(const uint8_t mac[6], uint8_t eui[8])
{
    eui[0] = mac[0];
    eui[1] = mac[1];
    eui[2] = mac[2];
    eui[3] = 0xFF;
    eui[4] = 0xFE;
    eui[5] = mac[3];
    eui[6] = mac[4];
    eui[7] = mac[5];
}

/* ---- JSON ---------------------------------------------------------------- */

static int rxpk_valid(const lwc_rxpk_t *p)
{
    return p->sf >= 5U && p->sf <= 12U &&
           (p->bw_khz == 125U || p->bw_khz == 250U || p->bw_khz == 500U) &&
           p->cr_denom >= 5U && p->cr_denom <= 8U && p->size <= 255U &&
           (p->payload != NULL || p->size == 0U) &&
           (p->stat == 1 || p->stat == -1 || p->stat == 0);
}

lwc_gwmp_err_t lwc_gwmp_rxpk_json(char *out, size_t cap, const lwc_rxpk_t *pk, size_t n,
                                  size_t *len)
{
    if (out == NULL || len == NULL || (pk == NULL && n > 0U) || cap == 0U) {
        return LWC_GWMP_ERR_ARG;
    }
    sbuf_t b = {out, cap, 0, 0};
    char b64[345]; /* 4 * ceil(255 / 3) + 1 */
    char ts[32];

    sb_printf(&b, "{\"rxpk\":[");
    for (size_t i = 0; i < n; i++) {
        const lwc_rxpk_t *p = &pk[i];
        if (!rxpk_valid(p)) {
            return LWC_GWMP_ERR_FIELD;
        }
        (void)lwc_base64_encode(p->payload, p->size, b64, sizeof b64);
        sb_printf(&b, "%s{", i ? "," : "");
        if (p->time_unix_us >= 0 && lwc_time_iso8601_us(p->time_unix_us, ts, sizeof ts) == 0) {
            sb_printf(&b, "\"time\":\"%s\",", ts);
        }
        sb_printf(&b,
                  "\"tmst\":%lu,\"chan\":%u,\"rfch\":%u,\"freq\":%lu.%06lu,\"stat\":%d,"
                  "\"modu\":\"LORA\",\"datr\":\"SF%uBW%u\",\"codr\":\"4/%u\","
                  "\"rssi\":%d,\"lsnr\":%.1f,\"size\":%u,\"data\":\"%s\"}",
                  (unsigned long)p->tmst, p->chan, p->rfch,
                  (unsigned long)(p->freq_hz / 1000000U), (unsigned long)(p->freq_hz % 1000000U),
                  p->stat, p->sf, p->bw_khz, p->cr_denom, p->rssi, (double)p->lsnr, p->size, b64);
    }
    sb_printf(&b, "]}");
    if (b.overflow) {
        return LWC_GWMP_ERR_SPACE;
    }
    *len = b.len;
    return LWC_GWMP_OK;
}

lwc_gwmp_err_t lwc_gwmp_stat_json(char *out, size_t cap, const lwc_stat_t *st, size_t *len)
{
    if (out == NULL || st == NULL || len == NULL || cap == 0U) {
        return LWC_GWMP_ERR_ARG;
    }
    sbuf_t b = {out, cap, 0, 0};
    char ts[32];
    sb_printf(&b, "{\"stat\":{");
    if (st->time_unix_s >= 0 && lwc_time_stat(st->time_unix_s, ts, sizeof ts) == 0) {
        sb_printf(&b, "\"time\":\"%s\",", ts);
    }
    const unsigned long ackr = st->ackr_pct > 100U ? 100UL : (unsigned long)st->ackr_pct;
    sb_printf(&b,
              "\"rxnb\":%lu,\"rxok\":%lu,\"rxfw\":%lu,\"ackr\":%lu.0,\"dwnb\":%lu,\"txnb\":%lu}}",
              (unsigned long)st->rxnb, (unsigned long)st->rxok, (unsigned long)st->rxfw, ackr,
              (unsigned long)st->dwnb, (unsigned long)st->txnb);
    if (b.overflow) {
        return LWC_GWMP_ERR_SPACE;
    }
    *len = b.len;
    return LWC_GWMP_OK;
}

/* ---- downstream parse ---------------------------------------------------- */

lwc_gwmp_err_t lwc_gwmp_parse_down(const uint8_t *buf, size_t len, lwc_gwmp_down_t *out)
{
    if (buf == NULL || out == NULL) {
        return LWC_GWMP_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (len < LWC_GWMP_HDR_DOWN) {
        return LWC_GWMP_ERR_LENGTH;
    }
    if (buf[0] != LWC_GWMP_VERSION) {
        return LWC_GWMP_ERR_VERSION;
    }
    out->token = (uint16_t)(((uint16_t)buf[1] << 8) | buf[2]);
    out->type = (lwc_gwmp_type_t)buf[3];
    switch (out->type) {
    case LWC_GWMP_PUSH_ACK:
    case LWC_GWMP_PULL_ACK:
        return LWC_GWMP_OK;
    case LWC_GWMP_PULL_RESP:
        if (len <= LWC_GWMP_HDR_DOWN) {
            return LWC_GWMP_ERR_LENGTH;
        }
        out->json = (const char *)&buf[LWC_GWMP_HDR_DOWN];
        out->json_len = len - LWC_GWMP_HDR_DOWN;
        return LWC_GWMP_OK;
    default:
        return LWC_GWMP_ERR_TYPE;
    }
}
