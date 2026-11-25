#include "lwc/gps.h"

#include <stdio.h>
#include <string.h>

/* ---- fields ---------------------------------------------------------------------- */

typedef struct {
    const char *p;
    size_t n;
} fld_t;

#define FIELDS_MAX 20U

/* Strict unsigned decimal "123" or "123.45" (no sign, no exponent). */
static bool dec(fld_t f, double *out)
{
    if (f.n == 0U || f.n > 15U) {
        return false;
    }
    double v = 0.0, scale = 1.0;
    bool dot = false, digit = false;
    for (size_t i = 0; i < f.n; i++) {
        const char c = f.p[i];
        if (c >= '0' && c <= '9') {
            digit = true;
            if (dot) {
                scale /= 10.0;
                v += (double)(c - '0') * scale;
            } else {
                v = v * 10.0 + (double)(c - '0');
            }
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    *out = v;
    return digit;
}

static bool two(const char *p, uint8_t *out, uint8_t max)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
        return false;
    }
    const uint8_t v = (uint8_t)((p[0] - '0') * 10 + (p[1] - '0'));
    if (v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* hhmmss or hhmmss.sss */
static bool time_of(fld_t f, lwc_gps_t *st)
{
    uint8_t h, m, s;
    if (f.n < 6U || !two(f.p, &h, 23) || !two(f.p + 2, &m, 59) || !two(f.p + 4, &s, 60)) {
        return false;
    }
    uint16_t ms = 0;
    if (f.n > 6U) {
        if (f.p[6] != '.' || f.n > 10U) {
            return false;
        }
        uint16_t scale = 100;
        for (size_t i = 7; i < f.n; i++) {
            if (f.p[i] < '0' || f.p[i] > '9') {
                return false;
            }
            ms = (uint16_t)(ms + (uint16_t)(f.p[i] - '0') * scale);
            scale /= 10U;
        }
    }
    st->hour = h;
    st->min = m;
    st->sec = s;
    st->ms = ms;
    st->time_ok = true;
    return true;
}

/* ddmmyy */
static bool date_of(fld_t f, lwc_gps_t *st)
{
    uint8_t d, mo, y;
    if (f.n != 6U || !two(f.p, &d, 31) || !two(f.p + 2, &mo, 12) || !two(f.p + 4, &y, 99) || d == 0U || mo == 0U) {
        return false;
    }
    st->day = d;
    st->mon = mo;
    st->year = (uint16_t)(y < 80U ? 2000U + y : 1900U + y); /* two digits: 80-99 are 1980-1999 */
    st->date_ok = true;
    return true;
}

/* ddmm.mmmm + N/S (or dddmm.mmmm + E/W) -> signed degrees */
static bool coord(fld_t v, fld_t hemi, bool lon, double *out)
{
    double x;
    if (hemi.n != 1U || !dec(v, &x)) {
        return false;
    }
    const double deg = (double)(long)(x / 100.0);
    const double min = x - deg * 100.0;
    if (min >= 60.0 || deg > (lon ? 180.0 : 90.0)) {
        return false;
    }
    double d = deg + min / 60.0;
    if (d > (lon ? 180.0 : 90.0)) {
        return false;
    }
    const char h = hemi.p[0];
    if (lon ? (h != 'E' && h != 'W') : (h != 'N' && h != 'S')) {
        return false;
    }
    *out = (h == 'S' || h == 'W') ? -d : d;
    return true;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* ---- sentences ------------------------------------------------------------------- */

static bool rmc(const fld_t *f, size_t nf, lwc_gps_t *st)
{
    if (nf < 10U) {
        return false;
    }
    lwc_gps_t t = *st; /* nothing changes unless the whole sentence is good */
    if (f[1].n > 0U && !time_of(f[1], &t)) {
        return false;
    }
    if (f[2].n != 1U || (f[2].p[0] != 'A' && f[2].p[0] != 'V')) {
        return false;
    }
    t.rmc_valid = f[2].p[0] == 'A';
    if (f[3].n > 0U || f[5].n > 0U) {
        double lat, lon;
        if (!coord(f[3], f[4], false, &lat) || !coord(f[5], f[6], true, &lon)) {
            return false;
        }
        t.lat = lat;
        t.lon = lon;
        t.pos_ok = true;
    } else if (!t.rmc_valid) {
        t.pos_ok = false; /* no fix */
    }
    if (f[9].n > 0U && !date_of(f[9], &t)) {
        return false;
    }
    if (t.rmc_valid && (!t.time_ok || f[1].n == 0U || f[9].n == 0U || !t.pos_ok)) {
        return false; /* status A must come with a time, a date and a position */
    }
    *st = t;
    return true;
}

static bool gga(const fld_t *f, size_t nf, lwc_gps_t *st)
{
    if (nf < 10U) {
        return false;
    }
    lwc_gps_t t = *st;
    double q = 0, sats = 0;
    if (f[6].n == 0U || !dec(f[6], &q) || q > 9.0) {
        return false;
    }
    if (f[7].n > 0U && (!dec(f[7], &sats) || sats > 99.0)) {
        return false;
    }
    t.quality = (uint8_t)q;
    t.sats = (uint8_t)sats;
    if (f[8].n > 0U && !dec(f[8], &t.hdop)) {
        return false;
    }
    if (t.quality > 0U) {
        double lat, lon;
        if (!coord(f[2], f[3], false, &lat) || !coord(f[4], f[5], true, &lon)) {
            return false;
        }
        t.lat = lat;
        t.lon = lon;
        t.pos_ok = true;
        /* altitude may be negative (below the geoid) */
        fld_t a = f[9];
        bool neg = false;
        if (a.n > 0U && a.p[0] == '-') {
            neg = true;
            a.p++;
            a.n--;
        }
        double alt;
        if (a.n > 0U && dec(a, &alt)) {
            t.alt_m = neg ? -alt : alt;
            t.alt_ok = true;
        } else if (f[9].n > 0U) {
            return false;
        }
    } else {
        t.alt_ok = false;
    }
    *st = t;
    return true;
}

lwc_nmea_t lwc_nmea_parse(const char *line, size_t n, lwc_gps_t *st)
{
    if (st == NULL) {
        return LWC_NMEA_BAD;
    }
    if (line == NULL) {
        st->bad++;
        return LWC_NMEA_BAD;
    }
    while (n > 0U && (line[n - 1U] == '\n' || line[n - 1U] == '\r')) {
        n--;
    }
    /* "$" body "*" hh */
    if (n < 7U || n > LWC_NMEA_MAX || line[0] != '$' || line[n - 3U] != '*') {
        st->bad++;
        return LWC_NMEA_BAD;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < n - 3U; i++) {
        const unsigned char c = (unsigned char)line[i];
        if (c < 0x20U || c > 0x7EU || c == '$' || c == '*') {
            st->bad++;
            return LWC_NMEA_BAD;
        }
        sum ^= c;
    }
    const int h1 = hexv(line[n - 2U]), h2 = hexv(line[n - 1U]);
    if (h1 < 0 || h2 < 0 || (uint8_t)(h1 * 16 + h2) != sum) {
        st->bad++;
        return LWC_NMEA_BAD;
    }
    fld_t f[FIELDS_MAX];
    size_t nf = 0;
    const char *p = line + 1, *end = line + n - 3U;
    while (nf < FIELDS_MAX) {
        const char *c = memchr(p, ',', (size_t)(end - p));
        const char *e = c != NULL ? c : end;
        f[nf++] = (fld_t){p, (size_t)(e - p)};
        if (c == NULL) {
            break;
        }
        p = c + 1;
    }
    /* address: talker (2) + type (3); proprietary "P..." and others ignored */
    if (f[0].n != 5U) {
        st->sentences++;
        return LWC_NMEA_OTHER;
    }
    lwc_nmea_t kind = LWC_NMEA_OTHER;
    bool ok = true;
    if (memcmp(f[0].p + 2, "RMC", 3) == 0) {
        kind = LWC_NMEA_RMC;
        ok = rmc(f, nf, st);
    } else if (memcmp(f[0].p + 2, "GGA", 3) == 0) {
        kind = LWC_NMEA_GGA;
        ok = gga(f, nf, st);
    }
    if (!ok) {
        st->bad++;
        return LWC_NMEA_BAD;
    }
    st->sentences++;
    return kind;
}

/* days since 1970-01-01 of a civil date (proleptic Gregorian) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2U ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153U * (m + (m > 2U ? (unsigned)-3 : 9U)) + 2U) / 5U + d - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t lwc_gps_unix(const lwc_gps_t *st)
{
    if (st == NULL || !st->time_ok || !st->date_ok) {
        return -1;
    }
    return days_from_civil(st->year, st->mon, st->day) * 86400 + (int64_t)st->hour * 3600 + (int64_t)st->min * 60 +
           st->sec;
}

size_t lwc_nmea_build(char *out, size_t cap, const char *body)
{
    if (out == NULL || body == NULL) {
        return 0;
    }
    uint8_t sum = 0;
    for (const char *c = body; *c != '\0'; c++) {
        sum ^= (uint8_t)*c;
    }
    const int n = snprintf(out, cap, "$%s*%02X\r\n", body, sum);
    return n > 0 && (size_t)n < cap && (size_t)n - 2U <= LWC_NMEA_MAX ? (size_t)n : 0U;
}

bool lwc_gps_clock_set(const lwc_gps_t *st, int64_t now_unix_s, lwc_clock_src_t src, int64_t sntp_age_s,
                       int64_t min_unix)
{
    if (st == NULL || !st->rmc_valid) {
        return false;
    }
    const int64_t u = lwc_gps_unix(st);
    if (u < 0 || u < min_unix || u >= 4102444800LL) { /* 2100-01-01 */
        return false;
    }
    if (src == LWC_CLOCK_SNTP && sntp_age_s >= 0 && sntp_age_s < LWC_GPS_SNTP_FRESH_S) {
        return false;
    }
    if (src == LWC_CLOCK_NONE) {
        return true;
    }
    const int64_t d = now_unix_s > u ? now_unix_s - u : u - now_unix_s;
    return d >= LWC_GPS_STEP_S;
}
