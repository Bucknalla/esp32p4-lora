#include "lwc/gps.h"

#include <math.h>
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

static lwc_nmea_t P(const char *s, lwc_gps_t *st)
{
    return lwc_nmea_parse(s, strlen(s), st);
}

static int near(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

static void test_sentences(void)
{
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    /* RMC, with CR LF */
    CHECK(P("$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*44\r\n", &st) == LWC_NMEA_RMC);
    CHECK(st.rmc_valid && st.time_ok && st.hour == 12U && st.min == 35U && st.sec == 19U && st.ms == 0U);
    CHECK(st.date_ok && st.day == 23U && st.mon == 3U && st.year == 1994U); /* 94: 1994 */
    CHECK(st.pos_ok && near(st.lat, 48.0 + 7.038 / 60.0) && near(st.lon, 11.0 + 31.0 / 60.0));
    /* any talker, south and west, fractions of a second */
    CHECK(P("$GNRMC,081836.75,A,3751.65,S,14507.36,W,000.0,360.0,251126,011.3,E,A*26", &st) == LWC_NMEA_RMC);
    CHECK(st.ms == 750U && st.year == 2026U && st.mon == 11U && st.day == 25U);
    CHECK(near(st.lat, -(37.0 + 51.65 / 60.0)) && near(st.lon, -(145.0 + 7.36 / 60.0)));
    /* GGA: quality, satellites, HDOP, altitude, a negative one */
    CHECK(P("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", &st) == LWC_NMEA_GGA);
    CHECK(st.quality == 1U && st.sats == 8U && near(st.hdop, 0.9) && st.alt_ok && near(st.alt_m, 545.4));
    CHECK(P("$GNGGA,081836.00,3751.65,S,14507.36,W,2,12,1.2,-12.5,M,,M,,*7E", &st) == LWC_NMEA_GGA);
    CHECK(st.quality == 2U && st.sats == 12U && near(st.alt_m, -12.5));
    /* no fix: empty fields; the time still counts, the fix does not */
    CHECK(P("$GPRMC,000001.00,V,,,,,,,251126,,,N*7F", &st) == LWC_NMEA_RMC);
    CHECK(!st.rmc_valid && !st.pos_ok && st.time_ok && st.sec == 1U && st.date_ok);
    CHECK(P("$GPGGA,000001.00,,,,,0,00,99.99,,,,,,*67", &st) == LWC_NMEA_GGA && st.quality == 0U && !st.alt_ok);
    CHECK(P("$GPRMC,235959,V,,,,,,,,,,N*52", &st) == LWC_NMEA_RMC && !st.rmc_valid);
    /* other sentences: good, ignored */
    const uint32_t good = st.sentences;
    CHECK(P("$PUBX,00,123519.00*3C", &st) == LWC_NMEA_OTHER);
    CHECK(P("$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74", &st) == LWC_NMEA_OTHER);
    CHECK(st.sentences == good + 2U);
}

static void test_refused(void)
{
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    CHECK(P("$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*44", &st) == LWC_NMEA_RMC);
    const lwc_gps_t before = st;
    static const char *const bad[] = {
        "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*45",  /* checksum */
        "GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*44",   /* no $ */
        "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W",     /* no checksum */
        "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*4G",  /* bad hex */
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,,003.1,W*65",           /* A without a date */
        "$GPRMC,123519,A,4807.038,X,01131.000,E,022.4,084.4,230394,003.1,W*7C",     /* hemisphere */
        "$GPRMC,123519,A,4860.000,N,01131.000,E,022.4,084.4,230394,003.1,W*60",     /* 60 minutes */
        "$GPRMC,246060,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*61",     /* 24 h */
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,320394,003.1,W*6A",     /* day 32 */
        "$GPRMC,123519,A,9100.000,N,01131.000,E,022.4,084.4,230394,003.1,W*62",     /* 91 degrees */
        "$GPRMC,123519,A,4807.0-8,N,01131.000,E,022.4,084.4,230394,003.1,W*74",     /* not a number */
        "$GPRMC,123519,X,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*73",     /* status */
        "", "$", "$*00", "$GPRMC*", "$GPRMC,1*00",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(P(bad[i], &st) == LWC_NMEA_BAD);
    }
    CHECK(st.bad == sizeof bad / sizeof bad[0]);
    /* a refused sentence changes nothing */
    CHECK(st.lat == before.lat && st.lon == before.lon && st.day == before.day && st.rmc_valid);
    /* too long (83 characters before CR LF) */
    char longs[100];
    memset(longs, 'A', sizeof longs);
    longs[0] = '$';
    longs[80] = '*';
    longs[81] = '0';
    longs[82] = '0';
    CHECK(lwc_nmea_parse(longs, 83, &st) == LWC_NMEA_BAD);
    CHECK(lwc_nmea_parse(NULL, 5, &st) == LWC_NMEA_BAD && lwc_nmea_parse("$x*00", 5, NULL) == LWC_NMEA_BAD);
    /* a control character inside */
    CHECK(P("$GP\x01RMC*00", &st) == LWC_NMEA_BAD);
}

static void test_build(void)
{
    char out[100];
    size_t n = lwc_nmea_build(out, sizeof out, "GPRMC,060000,A,5128.6740,N,00000.0900,W,0.0,0.0,060406,,,A");
    CHECK(n > 0U && strcmp(out, "$GPRMC,060000,A,5128.6740,N,00000.0900,W,0.0,0.0,060406,,,A*62\r\n") == 0);
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    CHECK(lwc_nmea_parse(out, n, &st) == LWC_NMEA_RMC && near(st.lat, 51.0 + 28.674 / 60.0) &&
          near(st.lon, -(0.09 / 60.0)) && st.year == 2006U);
    CHECK(lwc_nmea_build(out, 10, "GPRMC,060000") == 0U);
    char body[100];
    memset(body, 'X', 90);
    body[90] = '\0';
    CHECK(lwc_nmea_build(out, sizeof out, body) == 0U); /* longer than NMEA allows */
}

static void test_time(void)
{
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    CHECK(lwc_gps_unix(&st) == -1);
    CHECK(P("$GNRMC,081836.75,A,3751.65,S,14507.36,W,000.0,360.0,251126,011.3,E,A*26", &st) == LWC_NMEA_RMC);
    CHECK(lwc_gps_unix(&st) == 1795594716LL); /* 2026-11-25T08:18:36Z */
    st.year = 2000;
    st.mon = 2;
    st.day = 29;
    st.hour = 0;
    st.min = 0;
    st.sec = 0;
    CHECK(lwc_gps_unix(&st) == 951782400LL); /* leap day */
    st.year = 1970;
    st.mon = 1;
    st.day = 1;
    CHECK(lwc_gps_unix(&st) == 0);
}

static void test_clock(void)
{
    const int64_t MIN = 1764547200LL; /* 2025-12-01: the build */
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    CHECK(P("$GNRMC,081836.75,A,3751.65,S,14507.36,W,000.0,360.0,251126,011.3,E,A*26", &st) == LWC_NMEA_RMC);
    const int64_t T = lwc_gps_unix(&st);
    /* no clock: set it */
    CHECK(lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, MIN));
    /* SNTP fresh wins; SNTP quiet for an hour: GPS corrects beyond 2 s */
    CHECK(!lwc_gps_clock_set(&st, T + 30, LWC_CLOCK_SNTP, 10, MIN));
    CHECK(lwc_gps_clock_set(&st, T + 30, LWC_CLOCK_SNTP, LWC_GPS_SNTP_FRESH_S, MIN));
    CHECK(!lwc_gps_clock_set(&st, T + 1, LWC_CLOCK_SNTP, LWC_GPS_SNTP_FRESH_S, MIN));
    /* kept by GPS: corrected at 2 s, left alone below */
    CHECK(!lwc_gps_clock_set(&st, T + 1, LWC_CLOCK_GPS, -1, MIN));
    CHECK(lwc_gps_clock_set(&st, T + 2, LWC_CLOCK_GPS, -1, MIN));
    CHECK(lwc_gps_clock_set(&st, T - 2, LWC_CLOCK_GPS, -1, MIN));
    /* status V: never */
    st.rmc_valid = false;
    CHECK(!lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, MIN));
    st.rmc_valid = true;
    /* week rollover (a date ~19.6 years back) and nonsense: never */
    st.year = 2007;
    CHECK(!lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, MIN));
    st.year = 2100;
    CHECK(!lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, MIN));
    st.date_ok = false;
    st.year = 2026;
    CHECK(!lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, MIN));
    CHECK(!lwc_gps_clock_set(NULL, 0, LWC_CLOCK_NONE, -1, MIN));
}

/* 24 lines from go-gps-simulator (the owner's NMEA simulator: GGA, RMC,
 * GLL, VTG, GSA, GSV, ZDA, from no fix to a lock), run at a public test
 * position (Greenwich) */
static void test_simulator(void)
{
    static const char *const sim[] = {
        "$GPGGA,133030,,,,,0,00,,,,,,,,,*64",
        "$GPRMC,133030,V,,,,,,,,251126,,,N*7E",
        "$GPGLL,,,,,133030.78,V,N*47",
        "$GPVTG,,,,,,,,,N*30",
        "$GPGGA,133031,5128.6740,N,00000.0900,W,1,08,1.2,45.0,M,0.0,M,,*5B",
        "$GPRMC,133031,A,5128.6740,N,00000.0900,W,0.0,0.0,251126,,,A*60",
        "$GPGLL,5128.6740,N,00000.0900,W,133031.78,A,A*75",
        "$GPVTG,0.0,T,,M,0.0,N,0.0,K,A*0D",
        "$GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,2.1,1.2,1.8*33",
        "$GPGSV,2,1,08,01,64,277,18,02,37,238,33,03,36,077,25,04,22,117,30*74",
        "$GPGSV,2,2,08,05,34,005,22,06,66,268,28,07,61,332,41,08,49,091,19*74",
        "$GPZDA,133031.78,25,11,2026,00,00*6B",
        "$GPGGA,133032,5128.6740,N,00000.0900,W,1,08,1.2,45.0,M,0.0,M,,*58",
        "$GPRMC,133032,A,5128.6740,N,00000.0900,W,0.0,0.0,251126,,,A*63",
        "$GPGLL,5128.6740,N,00000.0900,W,133032.78,A,A*76",
        "$GPVTG,0.0,T,,M,0.0,N,0.0,K,A*0D",
        "$GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,2.1,1.2,1.8*33",
        "$GPGSV,2,1,08,01,65,278,17,02,37,237,35,03,35,076,24,04,21,117,32*7E",
        "$GPGSV,2,2,08,05,35,005,22,06,66,269,28,07,62,331,40,08,48,091,20*7E",
        "$GPZDA,133032.78,25,11,2026,00,00*68",
        "$GPGGA,133033,5128.6740,N,00000.0900,W,1,08,1.2,45.0,M,0.0,M,,*59",
        "$GPRMC,133033,A,5128.6740,N,00000.0900,W,0.0,0.0,251126,,,A*62",
        "$GPGLL,5128.6740,N,00000.0900,W,133033.78,A,A*77",
        "$GPVTG,0.0,T,,M,0.0,N,0.0,K,A*0D",
    };
    lwc_gps_t st;
    memset(&st, 0, sizeof st);
    unsigned rmc = 0, gga = 0, other = 0, bad = 0;
    bool was_v = false;
    for (size_t i = 0; i < sizeof sim / sizeof sim[0]; i++) {
        const lwc_nmea_t k = P(sim[i], &st);
        rmc += k == LWC_NMEA_RMC;
        gga += k == LWC_NMEA_GGA;
        other += k == LWC_NMEA_OTHER;
        bad += k == LWC_NMEA_BAD;
        if (k == LWC_NMEA_RMC && !st.rmc_valid) {
            was_v = true;
        }
    }
    CHECK(bad == 0U && rmc >= 2U && gga >= 2U && other > 0U && was_v);
    CHECK(st.rmc_valid && st.pos_ok && st.quality == 1U && st.sats == 8U && st.alt_ok && near(st.alt_m, 45.0));
    CHECK(fabs(st.lat - 51.4779) < 1e-4 && fabs(st.lon - -0.0015) < 1e-4 && st.year == 2026U);
}

int main(void)
{
    test_sentences();
    test_refused();
    test_build();
    test_time();
    test_clock();
    test_simulator();
    if (failures != 0) {
        fprintf(stderr, "test_gps: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gps: all passed\n");
    return 0;
}
