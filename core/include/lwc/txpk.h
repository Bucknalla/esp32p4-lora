/*
 * Downlink side of the Semtech UDP protocol: parse a PULL_RESP `txpk`, decide
 * when to send it (just-in-time queue), and keep the EU868 duty-cycle budget.
 * OS-independent, no allocation; the JSON is untrusted input.
 */
#ifndef LWC_TXPK_H
#define LWC_TXPK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- txpk ---------------------------------------------------------------- */

typedef enum {
    LWC_TXPK_TIMED = 0, /**< "tmst": concentrator counter */
    LWC_TXPK_IMMEDIATE, /**< "imme": true */
    LWC_TXPK_GPS,       /**< "tmms": GPS time (not supported here) */
} lwc_txpk_mode_t;

typedef struct {
    lwc_txpk_mode_t mode;
    uint32_t tmst;
    uint32_t freq_hz;
    uint8_t rfch;
    int8_t powe;      /**< dBm, rounded down */
    uint8_t sf;       /**< 7..12 */
    uint16_t bw_khz;  /**< 125, 250, 500 */
    uint8_t cr_denom; /**< 5..8 */
    bool ipol;
    bool ncrc;        /**< true: no payload CRC */
    uint16_t prea;    /**< preamble symbols, 0 = default */
    uint16_t size;
    uint8_t payload[255];
} lwc_txpk_t;

typedef enum {
    LWC_TXPK_OK = 0,
    LWC_TXPK_ERR_ARG,
    LWC_TXPK_ERR_JSON,    /**< malformed JSON or no "txpk" object */
    LWC_TXPK_ERR_FIELD,   /**< missing or out-of-range field */
    LWC_TXPK_ERR_MODU,    /**< not "LORA" */
} lwc_txpk_err_t;

/** Parse `{"txpk":{...}}`. Keys in any order, unknown keys skipped. */
lwc_txpk_err_t lwc_txpk_parse(const char *json, size_t len, lwc_txpk_t *out);

/** Decode base64 (padding optional). Returns bytes written, or -1. */
int lwc_base64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

/** LoRa time on air in microseconds (Semtech AN1200.13), rounded up.
 *  Low-data-rate optimisation on when a symbol lasts 16 ms or more
 *  (SF11/SF12 at 125 kHz, SF12 at 250 kHz). `preamble` in symbols, without
 *  the 4.25 sync symbols. Returns 0 for invalid parameters. */
uint32_t lwc_lora_airtime_us(uint8_t sf, uint16_t bw_khz, uint8_t cr_denom, uint16_t preamble,
                             uint16_t size, bool crc, bool implicit_header);

/* ---- just-in-time queue ---------------------------------------------------
 * Times are concentrator counter microseconds (wrapping uint32). Comparisons
 * use the signed 32-bit difference, valid within +/- 35 minutes. */

#define LWC_JIT_SLOTS 8U

typedef enum {
    LWC_JIT_OK = 0,
    LWC_JIT_TOO_LATE,
    LWC_JIT_TOO_EARLY,
    LWC_JIT_COLLISION,
    LWC_JIT_FULL,
} lwc_jit_res_t;

typedef struct {
    bool used;
    uint32_t start;
    uint32_t airtime_us;
    uint32_t id;
} lwc_jit_slot_t;

typedef struct {
    lwc_jit_slot_t slot[LWC_JIT_SLOTS];
    uint32_t min_lead_us;    /**< reject when start - now is below this */
    uint32_t max_advance_us; /**< reject when start - now is above this */
    uint32_t guard_us;       /**< spacing required between two frames */
} lwc_jit_t;

void lwc_jit_init(lwc_jit_t *q, uint32_t min_lead_us, uint32_t max_advance_us, uint32_t guard_us);

/** Admit a frame starting at `start` lasting `airtime_us`. On OK, `*slot` is its index. */
lwc_jit_res_t lwc_jit_admit(lwc_jit_t *q, uint32_t now, uint32_t start, uint32_t airtime_us,
                            uint32_t id, size_t *slot);

/** The earliest frame due within `lead_us` of `now`: returns its slot index,
 *  or -1. A frame already past `now + min_send_us` is reported through
 *  `*expired` (slot index, else -1) so the caller can drop it. */
int lwc_jit_due(const lwc_jit_t *q, uint32_t now, uint32_t lead_us, uint32_t min_send_us,
                int *expired);

void lwc_jit_release(lwc_jit_t *q, size_t slot);

/* ---- EU868 duty cycle (ETSI EN 300 220 sub-bands) -------------------------
 * Token bucket per sub-band over one hour: capacity = duty x 3600 s, refilled
 * continuously. Times are host monotonic microseconds (int64). */

#define LWC_DC_BANDS 6U

typedef struct {
    int64_t last_us;
    int64_t tokens_us[LWC_DC_BANDS];
} lwc_dutycycle_t;

void lwc_dc_init(lwc_dutycycle_t *dc, int64_t now_us);

/** Sub-band index for a frequency, or -1 outside the EU868 bands. */
int lwc_dc_band(uint32_t freq_hz);

/** Duty cycle of a band in per-mille (1 = 0.1 %, 10 = 1 %, 100 = 10 %). */
uint32_t lwc_dc_permille(int band);

/** True and consumed if `airtime_us` fits in the band's budget now. */
bool lwc_dc_take(lwc_dutycycle_t *dc, int64_t now_us, uint32_t freq_hz, uint32_t airtime_us);

#ifdef __cplusplus
}
#endif

#endif /* LWC_TXPK_H */
