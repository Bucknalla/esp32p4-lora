/* libFuzzer target: NMEA from a GPS receiver is untrusted input. */
#include "lwc/gps.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static lwc_gps_t st; /* carried over: sentences build on each other */
    const lwc_nmea_t k = lwc_nmea_parse((const char *)data, size, &st);
    /* what a good sentence leaves must be in range */
    if (k != LWC_NMEA_BAD &&
        ((st.time_ok && (st.hour > 23U || st.min > 59U || st.sec > 60U || st.ms > 999U)) ||
         (st.date_ok && (st.day < 1U || st.day > 31U || st.mon < 1U || st.mon > 12U || st.year < 1980U ||
                         st.year > 2079U)) ||
         (st.pos_ok && (!(st.lat >= -90.0 && st.lat <= 90.0) || !(st.lon >= -180.0 && st.lon <= 180.0))))) {
        __builtin_trap();
    }
    (void)lwc_gps_unix(&st);
    (void)lwc_gps_clock_set(&st, 0, LWC_CLOCK_NONE, -1, 0);
    char out[100];
    char body[90];
    const size_t n = size < sizeof body - 1U ? size : sizeof body - 1U;
    memcpy(body, data, n);
    body[n] = '\0';
    (void)lwc_nmea_build(out, sizeof out, body);
    return 0;
}
