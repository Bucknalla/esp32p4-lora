/* Downlink requests into the radio task, results back (Basics Station client). */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwc/txpk.h"

typedef enum {
    RADIO_TX_OK = 0,
    RADIO_TX_TOO_LATE,
    RADIO_TX_TOO_EARLY,
    RADIO_TX_COLLISION,
    RADIO_TX_FREQ,     /* outside the TX range, or TX disabled */
    RADIO_TX_GPS,      /* GPS-timed: no GPS on this gateway */
    RADIO_TX_DUTY,     /* sub-band duty-cycle budget used up */
    RADIO_TX_BUSY,     /* radio not running */
} radio_tx_result_t;

typedef enum {
    RADIO_SRC_LBS = 0, /* the Basics Station client: results and "on air" on g_radio_tx_res */
    RADIO_SRC_LNS = 1, /* the local network server: result logged only */
} radio_tx_src_t;

/* A second receive window to try if the first is refused (busy, duty cycle
 * used up, too late): the local server's RX2. Same payload, power and coding. */
typedef struct {
    bool on;
    uint32_t tmst;
    uint32_t freq_hz;
    uint8_t sf;
    uint16_t bw_khz;
} radio_tx_alt_t;

typedef struct {
    uint16_t token;   /* echoed in the result; 0 = console test */
    bool test;        /* trace and log only, no result expected */
    uint8_t source;   /* radio_tx_src_t: only RADIO_SRC_LBS results go to g_radio_tx_res */
    lwc_txpk_t pk;
    radio_tx_alt_t alt;
} radio_tx_req_t;

typedef struct {
    uint16_t token;
    radio_tx_result_t result;
    int8_t power_dbm; /* power actually used (after the cap) */
    bool power_capped;
    bool on_air;      /* the frame went to the radio (lgw_send): Basics Station dntxed */
    uint32_t tmst;    /* with on_air: its start on the concentrator counter */
} radio_tx_res_t;

#define RADIO_TX_QUEUE_LEN 4

/* Created by main before the radio and forwarder tasks start. */
extern QueueHandle_t g_radio_tx_req;
/* Results for RADIO_SRC_LBS: admission, then on_air when sent. */
extern QueueHandle_t g_radio_tx_res;

/* The concentrator counter, read by the radio task every loop (Basics Station
 * xtime needs it at least every 35 minutes). Not valid while the radio restarts. */
extern atomic_uint g_radio_counter;
extern atomic_bool g_radio_counter_ok;

typedef struct {
    atomic_uint txnb;    /* frames handed to lgw_send */
    atomic_uint txdrop;  /* admitted but dropped late */
} radio_tx_stats_t;

extern radio_tx_stats_t g_radio_tx_stats;

const char *radio_tx_result_str(radio_tx_result_t r);
