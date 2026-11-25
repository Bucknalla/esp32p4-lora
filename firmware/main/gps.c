/*
 * GPS receiver (ADR-0022). See gps.h.
 *
 * One task reads NMEA lines from the UART and parses them (core/src/gps.c).
 * A good fix may set the wall clock under the core rule (lwc_gps_clock_set):
 * SNTP wins. No PPS: the concentrator time reference (Class B, fine
 * timestamps) is not built yet. For tests without a receiver, an NMEA
 * simulator on a USB-to-UART adapter feeds the same pin.
 *
 * The position never goes to the log.
 */
#include "gps.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwc/gps.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "sv.h"

static const char *TAG = "gps";

#define NVS_NS "gps"
#define KEY_MODE "mode"
#define GPS_PORT UART_NUM_2
#define RX_BUF 1024
#define TASK_STACK 4096
#define TASK_PRIO 3
#define TASK_CORE 0
#define MIN_UNIX 1735689600LL /* 2025-01-01: older dates are a week rollover or a stale receiver */
#define WALL_VALID_S 1704067200LL /* 2024-01-01: below this the clock was never set */
#define UART_AVAILABLE (CONFIG_GW_GPS_UART_RX_GPIO >= 0)

static gps_mode_t s_mode = GPS_OFF;
static lwc_gps_t s_st;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_us = -1;  /* last good sentence */
static atomic_int s_clock_src = LWC_CLOCK_NONE;
static _Atomic int64_t s_sntp_us = -1;
static atomic_uint s_clock_sets;

/* ---- settings ---------------------------------------------------------------------- */

static const char *mode_name(gps_mode_t m)
{
    return m == GPS_UART ? "uart" : "off";
}

bool gps_mode_parse(const char *s, gps_mode_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "off") == 0) {
        *out = GPS_OFF;
    } else if (strcmp(s, "uart") == 0) {
        *out = GPS_UART;
    } else {
        return false;
    }
    return true;
}

static gps_mode_t stored(void)
{
    uint8_t m = GPS_OFF;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, KEY_MODE, &m);
        nvs_close(h);
    }
    /* a stored mode this build cannot run is off */
    if ((m == GPS_UART && !UART_AVAILABLE) || m > GPS_UART) {
        return GPS_OFF;
    }
    return (gps_mode_t)m;
}

esp_err_t gps_set_mode(gps_mode_t m)
{
    if (m == GPS_UART && !UART_AVAILABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(h, KEY_MODE, (uint8_t)m);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- clock --------------------------------------------------------------------------- */

void gps_sntp_synced(void)
{
    atomic_store(&s_clock_src, LWC_CLOCK_SNTP);
    atomic_store(&s_sntp_us, esp_timer_get_time());
}

static int64_t wall_s(void)
{
    struct timeval tv;
    return gettimeofday(&tv, NULL) == 0 ? (int64_t)tv.tv_sec : 0;
}

/* A good RMC from a real receiver: set or correct the wall clock if the rule says so. */
static void maybe_set_clock(const lwc_gps_t *st)
{
    const int64_t now = wall_s();
    int src = atomic_load(&s_clock_src);
    if (src == LWC_CLOCK_NONE && now >= WALL_VALID_S) {
        src = LWC_CLOCK_SNTP; /* set before this task knew (SNTP at boot) */
    }
    const int64_t sntp_us = atomic_load(&s_sntp_us);
    const int64_t sntp_age = sntp_us < 0 ? -1 : (esp_timer_get_time() - sntp_us) / 1000000;
    if (!lwc_gps_clock_set(st, now, (lwc_clock_src_t)src, sntp_age, MIN_UNIX)) {
        return;
    }
    const struct timeval tv = {.tv_sec = (time_t)lwc_gps_unix(st), .tv_usec = (suseconds_t)st->ms * 1000};
    if (settimeofday(&tv, NULL) == 0) {
        atomic_store(&s_clock_src, LWC_CLOCK_GPS);
        atomic_fetch_add(&s_clock_sets, 1U);
        ESP_LOGI(TAG, "wall clock set from GPS (it was %s)",
                 src == LWC_CLOCK_NONE ? "not set" : src == LWC_CLOCK_GPS ? "drifting" : "kept by an SNTP gone quiet");
    }
}

/* ---- the task ----------------------------------------------------------------------------- */

static void gps_task(void *arg)
{
    (void)arg;
    const sv_id_t sv = sv_register("gps", 30000);
    static char line[LWC_NMEA_MAX + 3U];
    static uint8_t buf[128];
    static lwc_gps_t work; /* this task's copy */
    size_t n = 0;
    bool overlong = false;
    for (;;) {
        sv_beat(sv);
        const int got = uart_read_bytes(GPS_PORT, buf, sizeof buf, pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            const char c = (char)buf[i];
            if (c == '$') {
                n = 0;
                overlong = false;
            }
            if (c == '\n') {
                if (n > 0U && !overlong) {
                    /* parsed here, outside the lock; only this task writes the state */
                    const lwc_nmea_t k = lwc_nmea_parse(line, n, &work);
                    taskENTER_CRITICAL(&s_mux);
                    s_st = work;
                    if (k != LWC_NMEA_BAD) {
                        s_last_us = esp_timer_get_time();
                    }
                    taskEXIT_CRITICAL(&s_mux);
                    if (k == LWC_NMEA_RMC) {
                        maybe_set_clock(&work);
                    }
                } else if (overlong) {
                    work.bad++;
                    taskENTER_CRITICAL(&s_mux);
                    s_st = work;
                    taskEXIT_CRITICAL(&s_mux);
                }
                n = 0;
                overlong = false;
            } else if (n < sizeof line) {
                line[n++] = c;
            } else {
                overlong = true;
            }
        }
    }
}

esp_err_t gps_start(void)
{
    s_mode = stored();
    if (s_mode == GPS_OFF) {
        return ESP_OK;
    }
    const uart_config_t cfg = {
        .baud_rate = CONFIG_GW_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(GPS_PORT, RX_BUF, 0, 0, NULL, 0), TAG, "driver");
    ESP_RETURN_ON_ERROR(uart_param_config(GPS_PORT, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(uart_set_pin(GPS_PORT, CONFIG_GW_GPS_UART_TX_GPIO, CONFIG_GW_GPS_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "pins");
    const BaseType_t ok = xTaskCreatePinnedToCore(gps_task, "gps", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "receiver on UART%d, %d baud", (int)GPS_PORT, CONFIG_GW_GPS_BAUD);
    return ESP_OK;
}

void gps_status_line(char *buf, size_t len)
{
    lwc_gps_t st;
    int64_t last;
    taskENTER_CRITICAL(&s_mux);
    st = s_st;
    last = s_last_us;
    taskEXIT_CRITICAL(&s_mux);
    const int src = atomic_load(&s_clock_src);
    const char *clock = src == LWC_CLOCK_GPS ? "gps" : src == LWC_CLOCK_SNTP || wall_s() >= WALL_VALID_S ? "sntp" : "none";
    if (s_mode == GPS_OFF) {
        snprintf(buf, len, "mode=off clock=%s", clock);
        return;
    }
    char pos[96] = "lat=- lon=- alt=-";
    if (st.pos_ok) {
        if (st.alt_ok) {
            snprintf(pos, sizeof pos, "lat=%.5f lon=%.5f alt=%.1f", st.lat, st.lon, st.alt_m);
        } else {
            snprintf(pos, sizeof pos, "lat=%.5f lon=%.5f alt=-", st.lat, st.lon);
        }
    }
    char utc[40] = "-";
    if (st.time_ok && st.date_ok) {
        snprintf(utc, sizeof utc, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.year, st.mon, st.day, st.hour, st.min, st.sec);
    }
    snprintf(buf, len,
             "mode=%s fix=%s quality=%u sats=%u %s hdop=%.1f utc=%s clock=%s clock_sets=%u sentences=%" PRIu32
             " bad=%" PRIu32 " last_s=%" PRId64 " pps=no",
             mode_name(s_mode), st.rmc_valid && st.pos_ok ? "yes" : "no", st.quality, st.sats, pos, st.hdop, utc, clock,
             atomic_load(&s_clock_sets), st.sentences, st.bad, last < 0 ? (int64_t)-1 : (esp_timer_get_time() - last) / 1000000);
}
