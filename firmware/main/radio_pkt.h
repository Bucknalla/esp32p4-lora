/* Radio -> forwarder queue item: one received packet in GWMP units. */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    int64_t time_unix_us; /* < 0 when the wall clock is not set */
    uint32_t tmst;
    uint32_t freq_hz;
    uint8_t chan;
    uint8_t rfch;
    int8_t stat; /* 1 CRC ok, -1 CRC bad, 0 no CRC */
    uint8_t sf;
    uint16_t bw_khz;
    uint8_t cr_denom;
    int16_t rssi;
    float lsnr;
    uint16_t size;
    uint8_t payload[256];
} radio_pkt_t;

#define RADIO_QUEUE_LEN 16

/* Counters owned by the radio task, read by the forwarder for `stat`. */
typedef struct {
    atomic_uint rxnb;    /* all packets from the concentrator */
    atomic_uint rxok;    /* CRC ok */
    atomic_uint dropped; /* queue full */
    atomic_uint restarts;
    atomic_uint starts; /* successful lgw_start calls */
} radio_stats_t;

extern radio_stats_t g_radio_stats;
