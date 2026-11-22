/*
 * LoRaWAN 1.0.x frames (LoRaWAN L2 1.0.4 specification): parse join-requests
 * and data frames, check and compute MICs, encrypt FRMPayload, derive session
 * keys, build join-accepts. OS-independent, no allocation; frames from the
 * air are untrusted input.
 *
 * Byte order: EUIs, DevAddr, NetID, nonces and counters are little-endian on
 * the wire; here they are plain integers (an EUI printed as 0102030405060708
 * is the value 0x0102030405060708). Keys are byte arrays in wire order.
 */
#ifndef LWC_LORAWAN_H
#define LWC_LORAWAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_LW_KEY_LEN 16U
#define LWC_LW_JOIN_REQ_LEN 23U
#define LWC_LW_MAX_FRAME 255U

typedef enum {
    LWC_LW_JOIN_REQUEST = 0,
    LWC_LW_JOIN_ACCEPT = 1,
    LWC_LW_UNCONFIRMED_UP = 2,
    LWC_LW_UNCONFIRMED_DOWN = 3,
    LWC_LW_CONFIRMED_UP = 4,
    LWC_LW_CONFIRMED_DOWN = 5,
    LWC_LW_REJOIN_REQUEST = 6,
    LWC_LW_PROPRIETARY = 7,
} lwc_lw_mtype_t;

typedef enum {
    LWC_LW_OK = 0,
    LWC_LW_ERR_ARG,    /**< NULL pointer or no room */
    LWC_LW_ERR_SHORT,  /**< too short (or wrong length) for its type */
    LWC_LW_ERR_MTYPE,  /**< not the frame type asked for */
    LWC_LW_ERR_MAJOR,  /**< not LoRaWAN R1 */
    LWC_LW_ERR_FORMAT, /**< FOpts and FPort 0 together */
} lwc_lw_err_t;

/* FCtrl bits */
#define LWC_LW_FCTRL_ADR 0x80U
#define LWC_LW_FCTRL_ADR_ACK_REQ 0x40U /* uplink */
#define LWC_LW_FCTRL_ACK 0x20U
#define LWC_LW_FCTRL_FPENDING 0x10U /* downlink; ClassB on uplink */
#define LWC_LW_FCTRL_FOPTS_LEN 0x0FU

/** MType of a frame (top 3 bits of MHDR), or -1 if empty. */
int lwc_lw_mtype(const uint8_t *p, size_t n);

typedef struct {
    uint64_t join_eui;
    uint64_t dev_eui;
    uint16_t dev_nonce;
} lwc_lw_join_req_t;

lwc_lw_err_t lwc_lw_join_req_parse(const uint8_t *p, size_t n, lwc_lw_join_req_t *out);

/** True if the join-request's MIC matches `app_key` (LoRaWAN 1.0: AppKey). */
bool lwc_lw_join_req_mic_ok(const uint8_t app_key[16], const uint8_t *p, size_t n);

typedef struct {
    lwc_lw_mtype_t mtype;
    uint32_t dev_addr;
    uint8_t fctrl;
    uint16_t fcnt16;
    uint8_t fopts_len;
    const uint8_t *fopts; /**< points into the frame */
    bool has_port;
    uint8_t fport;
    const uint8_t *frm;   /**< FRMPayload, encrypted; points into the frame */
    size_t frm_len;
} lwc_lw_data_t;

/** Parse a data frame (up or down, confirmed or not). The MIC is not checked. */
lwc_lw_err_t lwc_lw_data_parse(const uint8_t *p, size_t n, lwc_lw_data_t *out);

/** 32-bit frame counter from its low 16 bits: the nearest value >= `last`. */
uint32_t lwc_lw_fcnt32(uint32_t last, uint16_t fcnt16);

/** MIC of a data frame: `msg` is the PHYPayload without its 4 MIC bytes. */
void lwc_lw_data_mic(const uint8_t nwk_s_key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                     const uint8_t *msg, size_t len, uint8_t mic[4]);

/** True if the frame (with MIC) is authentic under `nwk_s_key` at `fcnt32`. */
bool lwc_lw_data_mic_ok(const uint8_t nwk_s_key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                        const uint8_t *p, size_t n);

/** Encrypt or decrypt FRMPayload (the same operation); `in` may equal `out`. */
void lwc_lw_crypt(const uint8_t key[16], bool downlink, uint32_t dev_addr, uint32_t fcnt32,
                  const uint8_t *in, size_t len, uint8_t *out);

/**
 * Build a data frame: MHDR, FHDR (FOptsLen is set here), optional FPort and
 * FRMPayload, MIC. `frm` is plaintext and is encrypted here, with `nwk_s_key`
 * on port 0 and `app_s_key` on other ports. The direction comes from `mtype`.
 * Returns the frame length, or 0 if it does not fit in `cap`, FOpts is over
 * 15 bytes, FOpts comes with port 0, or `mtype` is not a data frame.
 */
size_t lwc_lw_data_build(lwc_lw_mtype_t mtype, uint32_t dev_addr, uint8_t fctrl, uint32_t fcnt32,
                         const uint8_t *fopts, size_t fopts_len, bool has_port, uint8_t port,
                         const uint8_t *frm, size_t frm_len, const uint8_t nwk_s_key[16],
                         const uint8_t app_s_key[16], uint8_t *out, size_t cap);

/** LoRaWAN 1.0 session keys from AppKey, JoinNonce (24 bit), NetID (24 bit), DevNonce. */
void lwc_lw_session_keys(const uint8_t app_key[16], uint32_t join_nonce, uint32_t net_id,
                         uint16_t dev_nonce, uint8_t nwk_s_key[16], uint8_t app_s_key[16]);

/**
 * Build an encrypted LoRaWAN 1.0 join-accept PHYPayload. `cflist` is NULL or
 * 16 bytes. Returns its length (17 or 33), or 0 if `cap` is too small.
 */
size_t lwc_lw_join_accept(const uint8_t app_key[16], uint32_t join_nonce, uint32_t net_id,
                          uint32_t dev_addr, uint8_t dl_settings, uint8_t rx_delay,
                          const uint8_t *cflist, uint8_t *out, size_t cap);

/** EU868 CFList (type 0): five extra channel frequencies in Hz. */
void lwc_lw_cflist_freqs(const uint32_t freq_hz[5], uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* LWC_LORAWAN_H */
