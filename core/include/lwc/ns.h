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
} lwc_ns_dev_t;

typedef struct {
    lwc_ns_dev_t dev[LWC_NS_MAX_DEVICES];
    uint32_t join_nonce; /* next JoinNonce: increases on every join (24 bit) */
    uint32_t next_addr;  /* DevAddr allocator */
} lwc_ns_t;

void lwc_ns_init(lwc_ns_t *ns);

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
    LWC_NS_UP_BAD_MIC,  /* a device has the DevAddr, but the MIC fails: maybe a collision */
    LWC_NS_UP_REPLAY,   /* authentic, but the counter is not new */
    LWC_NS_UP_MALFORMED,
} lwc_ns_up_res_t;

typedef struct {
    size_t idx;
    uint32_t fcnt;
    bool confirmed;
    bool adr;
    bool has_port;
    uint8_t port;
    uint8_t payload[242]; /* decrypted FRMPayload */
    size_t len;
    uint8_t fopts[15];
    size_t fopts_len;
} lwc_ns_uplink_t;

lwc_ns_up_res_t lwc_ns_uplink(lwc_ns_t *ns, const uint8_t *frame, size_t n, lwc_ns_uplink_t *out);

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
 * RX1DROffset 0) or RX2 (869.525 MHz, DR0), `delay_us` after the uplink's
 * concentrator timestamp (one more second for RX2). Inverted polarity, no CRC.
 */
void lwc_ns_downlink(bool rx2, uint32_t up_tmst, uint32_t up_freq_hz, uint8_t up_sf, uint16_t up_bw_khz,
                     uint32_t delay_us, int8_t power_dbm, const uint8_t *payload, size_t len,
                     lwc_txpk_t *out);

/** EU868 join-accept settings: DLSettings (RX1DROffset 0, RX2 DR0), RxDelay 1 s, CFList 867.1-867.9 MHz. */
#define LWC_NS_DL_SETTINGS 0x00U
#define LWC_NS_RX_DELAY 1U

#ifdef __cplusplus
}
#endif

#endif /* LWC_NS_H */
