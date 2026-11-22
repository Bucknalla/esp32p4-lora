/*
 * Semtech UDP packet forwarder protocol (GWMP v2, as used by TTN and
 * ChirpStack). OS-independent: no allocation, fixed caller buffers, every
 * length checked. Downstream input is untrusted.
 *
 * Wire: [version=2][token hi][token lo][type][gateway EUI x8 (up only)][JSON]
 */
#ifndef LWC_GWMP_H
#define LWC_GWMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_GWMP_VERSION 2U
#define LWC_GWMP_HDR_UP 12U   /* with gateway EUI */
#define LWC_GWMP_HDR_DOWN 4U  /* without */

typedef enum {
    LWC_GWMP_PUSH_DATA = 0,
    LWC_GWMP_PUSH_ACK = 1,
    LWC_GWMP_PULL_DATA = 2,
    LWC_GWMP_PULL_RESP = 3,
    LWC_GWMP_PULL_ACK = 4,
    LWC_GWMP_TX_ACK = 5,
} lwc_gwmp_type_t;

/** One received LoRa packet, already converted from the HAL's units. */
typedef struct {
    int64_t time_unix_us; /**< UTC receive time; < 0 when the clock is not set */
    uint32_t tmst;        /**< concentrator counter, microseconds */
    uint32_t freq_hz;
    uint8_t chan;         /**< IF chain */
    uint8_t rfch;         /**< RF chain */
    int8_t stat;          /**< 1 CRC ok, -1 CRC bad, 0 no CRC */
    uint8_t sf;           /**< 7..12 */
    uint16_t bw_khz;      /**< 125, 250, 500 */
    uint8_t cr_denom;     /**< coding rate 4/5..4/8 -> 5..8 */
    int16_t rssi;         /**< dBm */
    float lsnr;           /**< dB */
    uint16_t size;
    const uint8_t *payload;
} lwc_rxpk_t;

typedef struct {
    int64_t time_unix_s; /**< < 0 when the clock is not set */
    uint32_t rxnb;       /**< packets received */
    uint32_t rxok;       /**< with a valid CRC */
    uint32_t rxfw;       /**< forwarded */
    uint32_t ackr_pct;   /**< upstream datagrams acknowledged, 0..100 */
    uint32_t dwnb;       /**< downlinks received */
    uint32_t txnb;       /**< packets transmitted */
} lwc_stat_t;

/** A parsed downstream datagram. `json` points into the input buffer. */
typedef struct {
    lwc_gwmp_type_t type;
    uint16_t token;
    const char *json; /**< PULL_RESP only, else NULL */
    size_t json_len;
} lwc_gwmp_down_t;

typedef enum {
    LWC_GWMP_OK = 0,
    LWC_GWMP_ERR_ARG,
    LWC_GWMP_ERR_SPACE,   /**< output buffer too small */
    LWC_GWMP_ERR_LENGTH,  /**< datagram too short */
    LWC_GWMP_ERR_VERSION,
    LWC_GWMP_ERR_TYPE,    /**< not a server-to-gateway type */
    LWC_GWMP_ERR_FIELD,   /**< rxpk field out of range */
} lwc_gwmp_err_t;

/** Write a header. `eui` NULL omits it (TX_ACK keeps it; PUSH/PULL_DATA need it).
 *  Returns bytes written, 0 if `cap` is too small. */
size_t lwc_gwmp_header(uint8_t *buf, size_t cap, lwc_gwmp_type_t type, uint16_t token,
                       const uint8_t eui[8]);

/** Append `{"rxpk":[...]}` for `n` packets. On success `*len` is the JSON length. */
lwc_gwmp_err_t lwc_gwmp_rxpk_json(char *out, size_t cap, const lwc_rxpk_t *pk, size_t n,
                                  size_t *len);

/** Append `{"stat":{...}}`. */
lwc_gwmp_err_t lwc_gwmp_stat_json(char *out, size_t cap, const lwc_stat_t *st, size_t *len);

/** Parse PUSH_ACK, PULL_ACK or PULL_RESP. */
lwc_gwmp_err_t lwc_gwmp_parse_down(const uint8_t *buf, size_t len, lwc_gwmp_down_t *out);

/** Standard base64 with padding, NUL-terminated. Returns chars written
 *  (without NUL), or 0 if `cap` is too small (and n > 0). */
size_t lwc_base64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/** Format UTC time as "YYYY-MM-DDTHH:MM:SS.uuuuuuZ" (28 chars + NUL).
 *  Returns 0 on success, -1 if `cap` < 28 or the time is out of range. */
int lwc_time_iso8601_us(int64_t unix_us, char *out, size_t cap);

/** Format as "YYYY-MM-DD HH:MM:SS GMT" (the `stat` time field). */
int lwc_time_stat(int64_t unix_s, char *out, size_t cap);

/** Gateway EUI-64 from a 48-bit MAC: MAC[0..2] FF FE MAC[3..5]. */
void lwc_eui64_from_mac(const uint8_t mac[6], uint8_t eui[8]);

#ifdef __cplusplus
}
#endif

#endif /* LWC_GWMP_H */
