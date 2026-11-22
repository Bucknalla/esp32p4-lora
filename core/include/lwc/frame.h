/*
 * LoRaWAN 1.0.4 PHYPayload parser.
 *
 * Part of the OS-independent core library: no ESP-IDF or FreeRTOS headers.
 * Input is untrusted (radio). The parser checks every length, never copies
 * and never allocates; the result points into the caller's buffer.
 */
#ifndef LWC_FRAME_H
#define LWC_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest PHYPayload any LoRaWAN region allows. */
#define LWC_PHY_MAX_LEN 255U
#define LWC_MIC_LEN 4U
#define LWC_FOPTS_MAX_LEN 15U

typedef enum {
    LWC_MTYPE_JOIN_REQUEST = 0,
    LWC_MTYPE_JOIN_ACCEPT = 1,
    LWC_MTYPE_UNCONFIRMED_UP = 2,
    LWC_MTYPE_UNCONFIRMED_DOWN = 3,
    LWC_MTYPE_CONFIRMED_UP = 4,
    LWC_MTYPE_CONFIRMED_DOWN = 5,
    LWC_MTYPE_REJOIN_REQUEST = 6,
    LWC_MTYPE_PROPRIETARY = 7,
} lwc_mtype_t;

typedef enum {
    LWC_FRAME_OK = 0,
    LWC_FRAME_ERR_ARG,        /**< NULL pointer */
    LWC_FRAME_ERR_LENGTH,     /**< too short, too long or wrong for the MType */
    LWC_FRAME_ERR_MAJOR,      /**< Major version is not LoRaWAN R1 */
    LWC_FRAME_ERR_UNSUPPORTED,/**< valid MType that v1 does not handle */
    LWC_FRAME_ERR_FOPTS_PORT0,/**< MAC commands in FOpts and FPort 0 together */
} lwc_frame_err_t;

/** FCtrl bits (uplink meaning; downlink reuses bit 4 as FPending). */
#define LWC_FCTRL_ADR 0x80U
#define LWC_FCTRL_ADR_ACK_REQ 0x40U
#define LWC_FCTRL_ACK 0x20U
#define LWC_FCTRL_CLASS_B 0x10U
#define LWC_FCTRL_FOPTS_LEN_MASK 0x0FU

typedef struct {
    uint32_t dev_addr;
    uint8_t fctrl;
    uint16_t fcnt16;        /**< low 16 bits of FCnt, as sent */
    const uint8_t *fopts;   /**< NULL when fopts_len is 0 */
    uint8_t fopts_len;
    int16_t fport;          /**< -1 when absent */
    const uint8_t *payload; /**< FRMPayload (still encrypted); NULL when absent */
    uint8_t payload_len;
} lwc_mac_payload_t;

typedef struct {
    uint64_t join_eui;
    uint64_t dev_eui;
    uint16_t dev_nonce;
} lwc_join_request_t;

typedef struct {
    const uint8_t *raw; /**< whole PHYPayload */
    size_t raw_len;
    uint8_t mhdr;
    lwc_mtype_t mtype;
    uint32_t mic;       /**< as received, little-endian decoded */
    union {
        lwc_mac_payload_t data;     /**< MType 2..5 */
        lwc_join_request_t join;    /**< MType 0 */
    } u;
} lwc_frame_t;

/**
 * Parse a PHYPayload.
 *
 * Join-accept, rejoin and proprietary frames return LWC_FRAME_ERR_UNSUPPORTED
 * with raw, mhdr and mtype filled in, so the caller can still forward them.
 */
lwc_frame_err_t lwc_frame_parse(const uint8_t *buf, size_t len, lwc_frame_t *out);

/** True for MType 2 and 4. */
int lwc_mtype_is_uplink_data(lwc_mtype_t mtype);

#ifdef __cplusplus
}
#endif

#endif /* LWC_FRAME_H */
