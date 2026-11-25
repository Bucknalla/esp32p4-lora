/*
 * GPS receiver input (ADR-0022): NMEA 0183 sentences (RMC, GGA) from any
 * talker (GP, GN, GL, GA, BD, ...), the checksum required; and the rule for
 * setting the wall clock from a fix. Pure parsing, no I/O: host-tested and
 * fuzzed.
 *
 * NMEA gives UTC. The wall clock is UTC as well (lwc_gps_from_unix_us adds the
 * leap seconds).
 */
#ifndef LWC_GPS_H
#define LWC_GPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_NMEA_MAX 82U /* the longest sentence, "$" to the checksum, without CR LF */

/** What the sentences so far have said. Fields not given stay as they were. */
typedef struct {
    /* RMC */
    bool time_ok;  /* hh mm ss read */
    uint8_t hour, min, sec;
    uint16_t ms;
    bool date_ok;
    uint8_t day, mon;
    uint16_t year;
    bool rmc_valid; /* status A: the receiver trusts its time and position */
    /* position (RMC or GGA) */
    bool pos_ok;
    double lat, lon; /* degrees, south and west negative */
    /* GGA */
    uint8_t quality; /* 0 none, 1 GPS, 2 DGPS, ... */
    uint8_t sats;
    bool alt_ok;
    double alt_m;
    double hdop;
    /* counters */
    uint32_t sentences; /* good sentences */
    uint32_t bad;       /* bad checksum or format */
} lwc_gps_t;

typedef enum {
    LWC_NMEA_BAD = 0, /* not a sentence, bad checksum or bad field */
    LWC_NMEA_RMC,
    LWC_NMEA_GGA,
    LWC_NMEA_OTHER,   /* a good sentence of another type: ignored */
} lwc_nmea_t;

/** Parse one sentence (`n` bytes, CR LF optional) into `st`. */
lwc_nmea_t lwc_nmea_parse(const char *line, size_t n, lwc_gps_t *st);

/** Unix seconds of the last RMC date and time, or -1 without both. */
int64_t lwc_gps_unix(const lwc_gps_t *st);

/** Build a sentence with its checksum and CR LF from the body between "$"
 *  and "*" (e.g. "GPRMC,..."). Returns its length, 0 if it does not fit. */
size_t lwc_nmea_build(char *out, size_t cap, const char *body);

typedef enum {
    LWC_CLOCK_NONE = 0, /* the wall clock is not set */
    LWC_CLOCK_SNTP,
    LWC_CLOCK_GPS,
} lwc_clock_src_t;

#define LWC_GPS_SNTP_FRESH_S 3600 /* SNTP within this: it wins */
#define LWC_GPS_STEP_S 2          /* a GPS-kept clock is corrected beyond this */

/**
 * Whether to set the wall clock from a GPS fix. Never without RMC status A,
 * never from a date before `min_unix` (an old receiver's week rollover gives
 * dates ~19.6 years back) or after 2099.
 * SNTP synced within LWC_GPS_SNTP_FRESH_S wins. A clock that is not set is
 * set; one kept by GPS (or by an SNTP that went quiet) is corrected when it
 * is LWC_GPS_STEP_S or more away.
 */
bool lwc_gps_clock_set(const lwc_gps_t *st, int64_t now_unix_s, lwc_clock_src_t src, int64_t sntp_age_s,
                       int64_t min_unix);

#ifdef __cplusplus
}
#endif

#endif /* LWC_GPS_H */
