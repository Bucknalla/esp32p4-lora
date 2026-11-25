/* GPS receiver (ADR-0022): NMEA over a UART. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    GPS_OFF = 0,
    GPS_UART,
} gps_mode_t;

/** Parse "off" | "uart"; false for anything else. */
bool gps_mode_parse(const char *s, gps_mode_t *out);

/** Store the mode for the next boot. ESP_ERR_NOT_SUPPORTED: "uart" without
 *  the GPS pins in this build. */
esp_err_t gps_set_mode(gps_mode_t m);

/** Start the receiver task for the stored mode (nothing when off). */
esp_err_t gps_start(void);

/** SNTP set the clock (the SNTP sync callback). */
void gps_sntp_synced(void);

/** mode=.. fix=yes|no quality=N sats=N lat=.. lon=.. alt=.. hdop=.. utc=.. clock=none|sntp|gps
 *  clock_sets=N sentences=N bad=N last_s=N pps=no */
void gps_status_line(char *buf, size_t len);
