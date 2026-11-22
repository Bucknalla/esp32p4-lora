/* Downlink requests into the radio task, results back (forwarder, console). */
#pragma once

#include <stdatomic.h>
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
    RADIO_SRC_UDP = 0, /* the Semtech UDP forwarder: result on g_radio_tx_res */
    RADIO_SRC_LNS = 1, /* the local network server: result logged only */
} radio_tx_src_t;

typedef struct {
    uint16_t token;   /* GWMP token, echoed in the result; 0 = console test */
    bool test;        /* trace and log only, no result expected */
    uint8_t source;   /* radio_tx_src_t: only the forwarder reads g_radio_tx_res */
    lwc_txpk_t pk;
} radio_tx_req_t;

typedef struct {
    uint16_t token;
    radio_tx_result_t result;
    int8_t power_dbm; /* power actually used (after the cap) */
    bool power_capped;
} radio_tx_res_t;

#define RADIO_TX_QUEUE_LEN 4

/* Created by main before the radio and forwarder tasks start. */
extern QueueHandle_t g_radio_tx_req;
extern QueueHandle_t g_radio_tx_res;

typedef struct {
    atomic_uint txnb;    /* frames handed to lgw_send */
    atomic_uint txdrop;  /* admitted but dropped late */
} radio_tx_stats_t;

extern radio_tx_stats_t g_radio_tx_stats;

const char *radio_tx_result_str(radio_tx_result_t r);
