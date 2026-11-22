/*
 * LoRaWAN 1.0.x MAC commands (L2 1.0.4 section 5, EU868 RP002): parse the
 * commands a device sends (FOpts or FRMPayload on port 0) and build the ones
 * the network server sends. OS-independent, no allocation; uplink commands are
 * untrusted input.
 */
#ifndef LWC_MAC_H
#define LWC_MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CIDs (the same number in both directions) */
#define LWC_MAC_LINK_CHECK 0x02U
#define LWC_MAC_LINK_ADR 0x03U
#define LWC_MAC_DUTY_CYCLE 0x04U
#define LWC_MAC_RX_PARAM_SETUP 0x05U
#define LWC_MAC_DEV_STATUS 0x06U
#define LWC_MAC_NEW_CHANNEL 0x07U
#define LWC_MAC_RX_TIMING_SETUP 0x08U
#define LWC_MAC_TX_PARAM_SETUP 0x09U
#define LWC_MAC_DL_CHANNEL 0x0AU
#define LWC_MAC_DEVICE_TIME 0x0DU

/* LinkADRAns status bits */
#define LWC_MAC_ADR_CHMASK_ACK 0x01U
#define LWC_MAC_ADR_DR_ACK 0x02U
#define LWC_MAC_ADR_POWER_ACK 0x04U
#define LWC_MAC_ADR_ALL_ACK 0x07U

/** What a device sent. Answers the server does not act on are counted only. */
typedef struct {
    bool link_check_req;
    bool device_time_req;
    uint8_t link_adr_ans; /* LinkADRAns count (a ChMask block answers once per request) */
    uint8_t link_adr_status; /* AND of their status bytes */
    bool dev_status_ans;
    uint8_t battery; /* 0 external power, 1..254 level, 255 unknown */
    int8_t margin;   /* demodulation margin, dB, -32..31 */
    uint8_t other;   /* other known commands (answers to requests this server does not send) */
    bool stopped;    /* an unknown CID or a short command ended parsing early */
} lwc_mac_up_t;

/**
 * Parse uplink MAC commands. Parsing stops at the first unknown CID or at a
 * command cut short (spec: the rest cannot be read); what came before it is
 * kept. Returns the number of commands read.
 */
size_t lwc_mac_parse_up(const uint8_t *p, size_t n, lwc_mac_up_t *out);

/* Builders append one command to buf[*len..cap) and return false if it does not fit. */
bool lwc_mac_link_check_ans(uint8_t *buf, size_t cap, size_t *len, uint8_t margin_db, uint8_t gw_cnt);
/** `gps_s`: whole seconds since the GPS epoch; `frac`: 1/256 s. */
bool lwc_mac_device_time_ans(uint8_t *buf, size_t cap, size_t *len, uint32_t gps_s, uint8_t frac);
/** `dr` 0..15, `tx_power` 0..15, `ch_mask` channels 0-15, `ch_mask_cntl` 0..7, `nb_trans` 0..15. */
bool lwc_mac_link_adr_req(uint8_t *buf, size_t cap, size_t *len, uint8_t dr, uint8_t tx_power, uint16_t ch_mask,
                          uint8_t ch_mask_cntl, uint8_t nb_trans);
bool lwc_mac_dev_status_req(uint8_t *buf, size_t cap, size_t *len);

/* GPS time: seconds since 1980-01-06T00:00:00Z, no leap seconds. */
#define LWC_GPS_EPOCH_UNIX 315964800LL
#define LWC_GPS_LEAP_S 18LL /* GPS - UTC since 2017-01-01 */

/**
 * GPS time from Unix time in microseconds (UTC). Returns false for a time
 * before 2017 (the clock is not set, or the leap-second count is wrong).
 */
bool lwc_gps_from_unix_us(int64_t unix_us, uint32_t *gps_s, uint8_t *frac);

#ifdef __cplusplus
}
#endif

#endif /* LWC_MAC_H */
