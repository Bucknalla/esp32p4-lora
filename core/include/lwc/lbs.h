/*
 * LoRa Basics Station LNS protocol (Semtech protocol 2), gateway side
 * (ADR-0016): the messages the gateway sends (version, jreq, updf, propdf,
 * dntxed) and the ones it reads (router_config, dnmsg). EU868 only. Messages
 * from the server are untrusted input. OS-independent, no allocation.
 *
 * Integers that can exceed 2^53 (xtime, diid, rctx) are read and written as
 * exact 64-bit integers, never through a double.
 */
#ifndef LWC_LBS_H
#define LWC_LBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwc/txpk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_LBS_PROTOCOL 2

typedef enum {
    LWC_LBS_OK = 0,
    LWC_LBS_ERR_ARG,
    LWC_LBS_ERR_SPACE,       /**< output buffer too small */
    LWC_LBS_ERR_FRAME,       /**< not a frame sent upstream (a downlink type, or too short) */
    LWC_LBS_ERR_DR,          /**< no EU868 data rate for this SF/BW */
    LWC_LBS_ERR_JSON,        /**< malformed JSON, or not an object */
    LWC_LBS_ERR_FIELD,       /**< a field missing, of the wrong type or out of range */
    LWC_LBS_ERR_UNSUPPORTED, /**< valid, but not supported here (class B, no xtime) */
    LWC_LBS_ERR_SESSION,     /**< xtime from another session (the radio restarted) */
} lwc_lbs_err_t;

/* ---- data rates (EU868, RP002) ---------------------------------------------- */

/** EU868 data rate for a LoRa SF/BW: 0..6, or -1. */
int lwc_lbs_dr(uint8_t sf, uint16_t bw_khz);
/** LoRa SF/BW of an EU868 data rate 0..6; false for any other. */
bool lwc_lbs_dr_params(uint8_t dr, uint8_t *sf, uint16_t *bw_khz);

/* ---- xtime ------------------------------------------------------------------ */

/*
 * xtime: bit 63 zero; bits 62-56 radio unit (always 0 here); bits 55-48 a
 * session number (never 0, new for every concentrator start); bits 47-0 the
 * concentrator microsecond counter, extended from its 32 bits.
 *
 * The 32-bit counter wraps every 71.6 minutes. Feed the clock the current
 * counter (lwc_lbs_xtime) more often than every 2^31 us (35.8 minutes; once a
 * minute is plenty), or it cannot tell forward from back. A counter slightly
 * older than the last one fed (an uplink received before the last sample)
 * maps behind it without moving the clock.
 */
typedef struct {
    uint8_t sess;
    bool have;
    uint32_t last;
    uint64_t now48; /**< 48-bit time of `last` */
} lwc_lbs_clock_t;

/** `sess` 1..255 (0 is replaced by 1). */
void lwc_lbs_clock_init(lwc_lbs_clock_t *c, uint8_t sess);
int64_t lwc_lbs_xtime(lwc_lbs_clock_t *c, uint32_t tmst);

/* ---- gateway to server ------------------------------------------------------ */

/** Radio data of one received frame. */
typedef struct {
    uint8_t sf;
    uint16_t bw_khz;
    uint32_t freq_hz;
    int64_t xtime;
    float rssi;       /**< dBm */
    float snr;        /**< dB */
    int64_t unix_us;  /**< reception time, < 0 when the clock is not set */
} lwc_lbs_rx_t;

/** `version` message; strings must not contain '"' or '\\'. */
lwc_lbs_err_t lwc_lbs_version(char *out, size_t cap, const char *station, const char *firmware,
                              const char *package, const char *model, size_t *len);

/** jreq (join-request), updf (data up) or propdf (proprietary) for a raw frame. */
lwc_lbs_err_t lwc_lbs_uplink(char *out, size_t cap, const uint8_t *phy, size_t n, const lwc_lbs_rx_t *rx,
                             size_t *len);

/** dntxed: `dev_eui` is the DevEui string of the dnmsg, echoed as it came. */
lwc_lbs_err_t lwc_lbs_dntxed(char *out, size_t cap, int64_t diid, const char *dev_eui, int64_t rctx,
                             int64_t xtime, int64_t unix_us, size_t *len);

/* ---- server to gateway ------------------------------------------------------ */

typedef enum {
    LWC_LBS_MSG_OTHER = 0, /**< anything else: ignore it (timesync, runcmd, rmtsh, ...) */
    LWC_LBS_MSG_ROUTER_CONFIG,
    LWC_LBS_MSG_DNMSG,
    LWC_LBS_MSG_DNSCHED,
} lwc_lbs_msg_t;

/** The msgtype of a message from the server. */
lwc_lbs_err_t lwc_lbs_msgtype(const char *json, size_t len, lwc_lbs_msg_t *type);

#define LWC_LBS_EUI_STR 40U

/** A class A downlink (dnmsg). */
typedef struct {
    char dev_eui[LWC_LBS_EUI_STR]; /**< as sent (hex digits, '-' and ':') */
    int64_t diid;
    uint8_t dclass;   /**< 0 class A, 2 class C answering an uplink */
    int64_t xtime;    /**< of the uplink */
    int64_t rctx;
    uint8_t rx_delay; /**< seconds; 0 and absent read as 1 */
    bool rx1;
    uint8_t rx1_dr;
    uint32_t rx1_freq;
    bool rx2;
    uint8_t rx2_dr;
    uint32_t rx2_freq;
    uint8_t priority;
    uint16_t len;
    uint8_t pdu[255];
} lwc_lbs_dnmsg_t;

lwc_lbs_err_t lwc_lbs_parse_dnmsg(const char *json, size_t len, lwc_lbs_dnmsg_t *out);

/**
 * Radio requests for a dnmsg: RX1 (if given) and RX2 (if given), timed on the
 * concentrator counter: xtime + RxDelay for RX1, one second later for RX2.
 * Only the session bits and the low 32 bits of xtime are used (the server may
 * keep its own count of counter wraps). Power is the EU868 default (16 dBm,
 * 27 dBm in 869.4-869.65 MHz); the radio applies its own cap.
 * `*first` is the window to try first, `*alt` the other one (alt->size 0: none).
 */
lwc_lbs_err_t lwc_lbs_dn_txpk(const lwc_lbs_dnmsg_t *d, uint8_t sess, lwc_txpk_t *first, lwc_txpk_t *alt);

/** What router_config says, as far as this gateway checks it. */
typedef struct {
    char region[16];
    uint32_t freq_min;
    uint32_t freq_max;
    bool drs_ok;           /**< DRs 0-6 match EU868 */
    uint32_t radio_hz[2];  /**< sx1301_conf[0] radio_0/1 freq (0: absent) */
    uint8_t multisf;       /**< bit i: chan_multiSF_i enabled */
    uint8_t chan_radio[8]; /**< chan_multiSF_i radio */
    int32_t chan_if[8];    /**< chan_multiSF_i IF offset, Hz */
} lwc_lbs_router_config_t;

lwc_lbs_err_t lwc_lbs_parse_router_config(const char *json, size_t len, lwc_lbs_router_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LWC_LBS_H */
