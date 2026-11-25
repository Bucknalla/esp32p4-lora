/*
 * Local network server and join server for one gateway (ADR-0015): a fixed
 * table of OTAA devices, join handling, uplink checks and the routing rule for
 * the gateway modes. LoRaWAN 1.0.x, EU868, class A. OS-independent, no
 * allocation, no I/O: the firmware stores the table and sends the frames.
 */
#ifndef LWC_NS_H
#define LWC_NS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwc/plan.h"

#include "lwc/txpk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_NS_MAX_DEVICES 32U
#define LWC_NS_NAME_MAX 23U
#define LWC_NS_NONCE_HISTORY 8U
#define LWC_NS_NET_ID 0x000000U      /* private / experimental NetID */
#define LWC_NS_ADDR_MAX 0x01FFFFFFU  /* NetID type 0, NwkID 0: 0x00000000-0x01FFFFFF */

typedef enum {
    LWC_NS_MAC_1_0_2 = 0, /* 1.0.2 / 1.0.3: random DevNonce, rejected if recently used */
    LWC_NS_MAC_1_0_4 = 1, /* 1.0.4: DevNonce is a counter, must increase */
} lwc_ns_mac_t;

/* Stored whole by the firmware (one NVS record per device): changing this
 * struct changes the record format. Runtime state goes in lwc_ns_link_t. */
typedef struct {
    bool used;
    char name[LWC_NS_NAME_MAX + 1U];
    uint64_t dev_eui;
    uint64_t join_eui;
    uint8_t app_key[16];
    uint8_t mac; /* lwc_ns_mac_t */
    /* nonces */
    uint16_t nonces[LWC_NS_NONCE_HISTORY]; /* recent DevNonces, newest at nonce_pos-1 */
    uint8_t nonce_count;
    uint8_t nonce_pos;
    /* session (valid when joined) */
    bool joined;
    uint32_t dev_addr;
    uint8_t nwk_s_key[16];
    uint8_t app_s_key[16];
    bool has_up;
    uint32_t fcnt_up;   /* last accepted uplink counter */
    uint32_t fcnt_down; /* next downlink counter */
    /* receive windows sent in its join-accept (record version 2, ADR-0017) */
    uint8_t rx1_delay_s; /* RX1 delay, 1..15 s */
    uint8_t rx2_dr;      /* RX2 data rate, EU868 DR0..DR5 */
    uint32_t rx2_hz;     /* RX2 frequency */
} lwc_ns_dev_t;

#define LWC_NS_RETRANS_MAX 7U        /* ACKs again for one repeated confirmed uplink */
#define LWC_NS_STATUS_PERIOD_S 86400U /* DevStatusReq at most once a day, with another downlink */

/* ADR (after ChirpStack's default algorithm, EU868 DR0-DR5 at 125 kHz) */
#define LWC_NS_ADR_HISTORY 20U     /* SNR history; a full one is needed before raising the power */
#define LWC_NS_ADR_MARGIN10 100    /* installation margin, 0.1 dB */
#define LWC_NS_ADR_MAX_DR 5U       /* SF7 */
#define LWC_NS_ADR_MAX_POWER 5U    /* TXPower index: MaxEIRP - 10 dB (RP002 allows 7; kept low
                                      for older regional parameter versions) */
#define LWC_NS_CH_MASK 0x00FFU     /* channels 0-7: three defaults and the CFList */

/* Per-device state that is not stored: it is rebuilt after a restart. */
typedef struct {
    uint8_t retrans;     /* repeats of the last uplink seen */
    bool status_asked;   /* DevStatusReq sent at status_req_s */
    uint32_t status_req_s;
    bool has_status;     /* last DevStatusAns */
    uint8_t battery;     /* 0 external, 1..254, 255 unknown */
    int8_t margin;       /* dB */
    uint32_t status_s;
    /* ADR */
    int16_t snr10[LWC_NS_ADR_HISTORY];
    uint8_t snr_n;       /* entries in snr10 */
    uint8_t snr_pos;
    uint8_t tx_power;    /* TXPower index the device uses, as far as known (0: max, the default) */
    uint8_t power_cap;   /* 0: LWC_NS_ADR_MAX_POWER; else 1 + the highest index the device accepts */
    uint8_t dr_cap;      /* 0: LWC_NS_ADR_MAX_DR; else 1 + the highest data rate the device accepts */
    bool adr_pending;    /* LinkADRReq sent, answer not seen yet */
    uint8_t pend_dr;
    uint8_t pend_power;
    uint8_t adr_wait;    /* uplinks to wait before asking again */
    uint8_t adr_nacks;
    uint8_t dr_kept;     /* accepted data-rate changes in a row that the device did not use */
} lwc_ns_link_t;

typedef struct {
    lwc_ns_dev_t dev[LWC_NS_MAX_DEVICES];
    lwc_ns_link_t link[LWC_NS_MAX_DEVICES];
    uint32_t join_nonce; /* next JoinNonce: increases on every join (24 bit) */
    uint32_t next_addr;  /* DevAddr allocator */
    uint32_t cflist_hz[5]; /* join-accept CFList, from the radio plan (lwc_ns_set_plan) */
    uint8_t rx1_delay_s;   /* receive windows for new joins (lwc_ns_set_rx) */
    uint8_t rx2_dr;
    uint32_t rx2_hz;
} lwc_ns_t;

/** Empty table; the CFList of the default EU868 plan. */
void lwc_ns_init(lwc_ns_t *ns);

/** The CFList of new join-accepts follows `plan` (ADR-0017). Devices already
 *  joined keep the channels of their join-accept until they join again. */
void lwc_ns_set_plan(lwc_ns_t *ns, const lwc_plan_t *plan);

/** True for a usable setting: RX1 delay 1..15 s, RX2 DR0..DR5, RX2 in 863-870 MHz. */
bool lwc_ns_rx_ok(uint8_t rx1_delay_s, uint32_t rx2_hz, uint8_t rx2_dr);

/** Receive windows for new joins (default: RX1 1 s, RX2 869.525 MHz DR0).
 *  Joined devices keep the ones of their join-accept. False if not usable. */
bool lwc_ns_set_rx(lwc_ns_t *ns, uint8_t rx1_delay_s, uint32_t rx2_hz, uint8_t rx2_dr);

typedef enum {
    LWC_NS_OK = 0,
    LWC_NS_ERR_ARG,
    LWC_NS_ERR_FULL,
    LWC_NS_ERR_EXISTS,
    LWC_NS_ERR_NOT_FOUND,
} lwc_ns_err_t;

/** Add an OTAA device (`name` may be NULL). Returns its index in *idx. */
lwc_ns_err_t lwc_ns_add(lwc_ns_t *ns, uint64_t dev_eui, uint64_t join_eui, const uint8_t app_key[16],
                        lwc_ns_mac_t mac, const char *name, size_t *idx);
lwc_ns_err_t lwc_ns_remove(lwc_ns_t *ns, uint64_t dev_eui);
/** Index of the device, or -1. */
int lwc_ns_find(const lwc_ns_t *ns, uint64_t dev_eui);

/* ---- join ----------------------------------------------------------------- */

typedef enum {
    LWC_NS_JOIN_ACCEPT = 0,  /* accept built; store the device, then send it */
    LWC_NS_JOIN_NOT_OURS,    /* no device with that DevEUI and JoinEUI */
    LWC_NS_JOIN_BAD_MIC,     /* our DevEUI, wrong key or corrupt frame */
    LWC_NS_JOIN_REPLAY,      /* DevNonce already used (or not increasing on 1.0.4) */
    LWC_NS_JOIN_MALFORMED,
    LWC_NS_JOIN_NO_ADDR,     /* no free DevAddr */
} lwc_ns_join_res_t;

/**
 * Handle a join-request. On LWC_NS_JOIN_ACCEPT the device has a new session
 * and *accept holds the encrypted join-accept (`cap` >= 33).
 */
lwc_ns_join_res_t lwc_ns_join(lwc_ns_t *ns, const uint8_t *frame, size_t n, uint8_t *accept,
                              size_t cap, size_t *accept_len, size_t *idx);

/* ---- uplink --------------------------------------------------------------- */

typedef enum {
    LWC_NS_UP_OK = 0,
    LWC_NS_UP_NOT_OURS, /* no joined device has that DevAddr */
    LWC_NS_UP_BAD_MIC,  /* a device has the DevAddr, but the MIC fails: maybe a collision, or an
                           old frame (its counter is read as the next 16-bit epoch) */
    LWC_NS_UP_REPLAY,   /* authentic, but the counter is not new (and not a repeat to answer) */
    LWC_NS_UP_MALFORMED,
    LWC_NS_UP_RETRANS,  /* the last frame again (a device without its ACK, or NbTrans > 1):
                           answer it again, do not deliver the payload twice */
} lwc_ns_up_res_t;

typedef struct {
    size_t idx;
    uint32_t fcnt;
    bool confirmed;
    bool adr;
    bool adr_ack_req;
    bool ack;         /* FCtrl.ACK: the device acknowledges the last confirmed downlink */
    bool has_port;
    uint8_t port;
    uint8_t payload[242]; /* decrypted FRMPayload */
    size_t len;
    uint8_t fopts[15];
    size_t fopts_len;
} lwc_ns_uplink_t;

/**
 * Check an uplink. On LWC_NS_UP_OK the payload is decrypted into *out; on
 * LWC_NS_UP_RETRANS only the header fields are set (no payload).
 */
lwc_ns_up_res_t lwc_ns_uplink(lwc_ns_t *ns, const uint8_t *frame, size_t n, lwc_ns_uplink_t *out);

/* ---- answer (class A downlink) -------------------------------------------- */

/** How the uplink was received. */
typedef struct {
    int16_t snr10;   /* SNR in 0.1 dB */
    uint8_t sf;
    uint16_t bw_khz; /* ADR runs at 125 kHz only */
    bool time_ok;    /* gps_s/gps_frac hold the uplink's GPS time */
    uint32_t gps_s;
    uint8_t gps_frac;
    uint32_t now_s;  /* monotonic seconds */
} lwc_ns_rx_t;

#define LWC_NS_DL_MAX 256U /* MHDR + FHDR with 15 B FOpts + FPort + 222 B + MIC = 250 */

typedef struct {
    uint8_t frame[LWC_NS_DL_MAX];
    size_t len;
    uint32_t fcnt;
    /* what is in it, for the log */
    bool ack;
    bool link_check;
    bool device_time;
    bool dev_status;
    bool link_adr;   /* LinkADRReq to adr_dr / adr_power */
    bool app;        /* the application payload is in it */
    bool confirmed;  /* sent as Confirmed Data Down (with the payload) */
    bool app_too_big; /* it was offered, but does not fit at these data rates */
    uint8_t adr_dr;
    uint8_t adr_power;
} lwc_ns_dl_t;

/**
 * After LWC_NS_UP_OK or LWC_NS_UP_RETRANS: read the device's MAC commands
 * and decide whether it needs a downlink now (ACK for a confirmed uplink,
 * ADRACKReq, LinkCheckReq, DeviceTimeReq, an ADR change). If so, build it (MAC answers in
 * FOpts, no FRMPayload), advance the downlink counter and return true: store
 * the counter before the frame goes out. A repeated uplink gets its ACK
 * again; its MAC commands are not read again.
 */
bool lwc_ns_answer(lwc_ns_t *ns, lwc_ns_up_res_t res, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                   lwc_ns_dl_t *dl);

/** An application downlink to send with the answer (ADR-0018). */
typedef struct {
    uint8_t port; /* 1..223 */
    const uint8_t *data;
    size_t len;
    bool more;      /* more are queued: sets FPending */
    bool confirmed; /* Confirmed Data Down: the device sets ACK in its next uplink */
} lwc_ns_app_t;

/**
 * lwc_ns_answer, with an optional application payload (NULL: none). A payload
 * makes a downlink needed. It is sent only if it fits both receive windows
 * (EU868 maximum FRMPayload for the uplink's data rate in RX1 and the device's
 * RX2 data rate, less the MAC commands); otherwise dl->app_too_big is set and
 * the answer is built as without it. The daily DevStatusReq rides along only
 * if it still fits.
 */
bool lwc_ns_answer_app(lwc_ns_t *ns, lwc_ns_up_res_t res, const lwc_ns_uplink_t *up, const lwc_ns_rx_t *rx,
                       const lwc_ns_app_t *app, lwc_ns_dl_t *dl);

/** EU868 data rate of an SF and bandwidth (DR0-DR6), or -1. */
int lwc_ns_dr(uint8_t sf, uint16_t bw_khz);

/** EU868 maximum FRMPayload with no FOpts (RP002 "N"), 0 for an unknown data rate. */
size_t lwc_ns_max_frm(int dr);

/** Demodulation floor for an SF at 125 kHz, in 0.1 dB (SF7 -7.5 dB ... SF12 -20 dB). */
int16_t lwc_ns_required_snr10(uint8_t sf);

/**
 * ADR step (as ChirpStack's default algorithm): from the best SNR of the
 * history, the data rate in use and the TXPower index, the data rate and
 * TXPower index to ask for. Steps are the spare margin over 3 dB, truncated
 * toward zero. Spare margin raises the data rate first, then lowers the power;
 * a shortfall raises the power only (the device lowers its data rate itself).
 */
void lwc_ns_adr(int16_t snr_max10, uint8_t dr, uint8_t tx_power, uint8_t max_dr, uint8_t max_power,
                uint8_t *new_dr, uint8_t *new_power);

/* ---- modes ---------------------------------------------------------------- */

typedef enum {
    LWC_MODE_TTN = 0,        /* forward everything */
    LWC_MODE_STANDALONE = 1, /* local server only */
    LWC_MODE_HYBRID = 2,     /* local devices here, the rest to TTN */
} lwc_mode_t;

/**
 * Where a received frame goes: true = forward it upstream. Frames the local
 * server accepted (join or uplink) stay local; in hybrid mode everything else
 * is forwarded, including a DevAddr match with a bad MIC.
 */
bool lwc_ns_forward(lwc_mode_t mode, bool handled_locally);

/* ---- downlink timing (EU868, RP002) --------------------------------------- */

#define LWC_NS_RX1_DELAY_US 1000000U
#define LWC_NS_JOIN_DELAY_US 5000000U
#define LWC_NS_RX2_FREQ_HZ 869525000U
#define LWC_NS_RX2_SF 12U /* DR0 */

/**
 * A class A downlink in RX1 (same frequency and data rate as the uplink,
 * RX1DROffset 0) or RX2 (`rx2_hz` at EU868 `rx2_dr`), `delay_us` after the
 * uplink's concentrator timestamp (one more second for RX2). Inverted
 * polarity, no CRC. A join-accept uses the default RX2 (LWC_NS_RX2_FREQ_HZ,
 * DR0): the device does not know other values yet.
 */
void lwc_ns_downlink(bool rx2, uint32_t up_tmst, uint32_t up_freq_hz, uint8_t up_sf, uint16_t up_bw_khz,
                     uint32_t delay_us, uint32_t rx2_hz, uint8_t rx2_dr, int8_t power_dbm, const uint8_t *payload,
                     size_t len, lwc_txpk_t *out);

/** RX1 delay of a joined device, microseconds. */
uint32_t lwc_ns_rx1_delay_us(const lwc_ns_dev_t *d);

#ifdef __cplusplus
}
#endif

#endif /* LWC_NS_H */
